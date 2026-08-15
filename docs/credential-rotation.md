# Credential and certificate rotation

This runbook covers routine and emergency rotation of VEKTOR's mutual-TLS
certificates, authorization identities, rollout approval keys, and artifact
trust keys. Test every procedure in a non-production fleet first. Keep fleet
time synchronized because certificate and approval validity checks depend on
UTC time.

## Reload behavior

VEKTOR does not currently expose a live-reload signal. Plan around these reload
boundaries:

| Material | Where it is configured | When it is read | Required action |
| --- | --- | --- | --- |
| Agent server certificate and private key | `--tls-cert`, `--tls-key` | Agent startup | Rolling agent restart |
| CA trusted for fleet client certificates | Agent `--tls-ca` | Agent startup | Rolling agent restart |
| Fleet client certificate, private key, and agent CA | Fleet inventory `transport` | Once per fleet poll | Start a new command or wait for the next watch poll |
| Authorization policy | `--authorization-policy` | Agent startup | Rolling agent restart |
| Cosign artifact trust policy | `--trust-policy` | Agent startup | Rolling agent restart |
| Approval policy, records, and approver public keys | Rollout command and each wave execution | Each execution | Start or resume the rollout command after atomic replacement |

A file replacement alone does not update an already running agent. Existing
gRPC connections can also outlive a client-side file replacement; use a new
fleet command when validating a rotation.

## Preparation and handling

Before any planned rotation:

1. Inventory certificate subjects, SANs, issuers, serial numbers, expiration
   dates, and the VEKTOR identity each certificate produces.
2. Confirm the new certificate's chain, hostname or `tls_server_name`, client or
   server extended-key usage, and public/private key match.
3. Back up the last known-good public material and encrypted recovery copies of
   private material. Do not retain a key known or suspected to be compromised.
4. Store private keys outside the repository and rollout YAML. Restrict them to
   the service account (for example, mode `0600` on Linux) or use a supported
   secret manager or KMS.
5. Replace files atomically on the same filesystem: write a new restricted
   temporary file, validate it, then rename it over the configured path.
6. Choose one healthy canary agent and set a maximum unavailable count of one.
   Do not continue while the canary is unreachable, unhealthy, or unauthorized.

Useful preflight checks:

```bash
openssl x509 -in agent.crt -noout -subject -issuer -serial -dates \
  -ext subjectAltName -ext extendedKeyUsage
openssl verify -CAfile agent-ca-bundle.crt agent.crt

openssl x509 -in agent.crt -pubkey -noout \
  | openssl pkey -pubin -outform DER | openssl sha256
openssl pkey -in agent.key -pubout -outform DER | openssl sha256
```

The last two digests must match. Supply intermediate certificates as required
by your PKI. OpenSSL's `verify` command builds and validates the certificate
chain; production issuance and revocation policy remains the responsibility of
your PKI.

## Rotate a leaf certificate

### Agent server certificate

1. Issue a new server certificate whose SAN still matches each inventory
   endpoint or configured `tls_server_name`.
2. Install the new certificate chain and private key atomically on one agent.
   Keep the current agent CA trusted by the fleet client.
3. Restart that agent. File replacement without restart is not sufficient.
4. From a new control-plane process, verify the canary:

   ```bash
   ros2 run vektor vektor fleet --config config/fleet.yaml --format json
   ```

5. Confirm the robot is reachable and healthy, then repeat one agent at a time.
6. After all agents are validated, retire the old leaf certificate and securely
   remove its private key according to your PKI policy.

### Fleet client certificate

The certificate identity is an authorization input. If the identity changes,
complete the identity migration below before switching certificates.

1. Issue and validate the new client certificate and key.
2. Atomically replace `client_certificate` and `client_key` on the control
   plane. A new `vektor fleet`, `deploy`, `promote`, or `rollback` command reads
   them immediately; watch mode reads them on its next poll.
3. Run `vektor fleet --format json` against the canary and then the complete
   target fleet. Check agent audit records for the expected `mtls:<identity>`.
4. Retire the old certificate only after every agent accepts the new one.

## Rotate a CA without an outage

Use an overlap period in which both old and new roots are trusted. A PEM bundle
may contain both CA certificates. Rotate one trust direction at a time.

### CA that signs agent server certificates

1. Put the old and new agent CAs in the fleet inventory's `ca_certificate` PEM
   bundle and validate with a new fleet command.
2. Rotate each agent server leaf to the new CA with the rolling procedure above.
3. Verify the entire fleet from a new command.
4. Remove the old agent CA from the fleet bundle and verify again.

### CA that signs fleet client certificates

1. Put the old and new client CAs in every agent's `--tls-ca` PEM bundle.
2. Restart and verify agents one at a time. The old client certificate remains
   valid during this phase.
3. Switch the fleet client certificate and key to a leaf signed by the new CA,
   then verify every authorization role and scope used by operations.
4. Remove the old client CA from each agent and perform another rolling restart.
5. Verify the fleet after each restart before advancing.

Do not rotate both directions simultaneously: it makes a failed handshake
ambiguous and makes rollback harder.

## Change a client identity or authorization scope

VEKTOR authorizes the authenticated client certificate identity exactly as it
appears in the schema-2 authorization policy.

1. Add the new identity with the minimum required roles, fleets, and workloads
   while retaining the old identity temporarily.
2. Roll out the policy with a one-agent-at-a-time restart.
3. Switch the client certificate and confirm inspect, deploy, promote, and
   rollback paths appropriate to that identity. Denials should retain the
   stable `VEKTOR_AUTHORIZATION_DENIED` body.
4. Remove the old identity from the policy and roll the policy out again.

Never grant `*` merely to simplify a rotation. Keep the overlap short and audit
both identities during the transition.

## Rotate rollout approval keys

Approval policy and public-key files are read for each rollout execution. Each
approver identity and each public-key path must be unique.

For a normal staged rotation:

1. Generate the new private key on the approver's trusted system. Distribute
   only its public key to the control plane.
2. Add it to the policy under a temporary new identity and a distinct path.
   Ensure `required_approvals` can still be met throughout the overlap.
3. Generate new `vektor approval-payload` output and signatures for the exact
   pending deployment, digest, fleet, workload, environment, and wave.
4. Atomically replace the approval bundle, run or resume the wave, and confirm
   the expected identities are reported.
5. Remove the old approver entry and old public key after outstanding approvals
   have expired or been replaced.

If the human approver identity must stay unchanged, the policy cannot contain
both keys under that identity. Atomically replace the public-key file and
regenerate every unexecuted approval signed by that identity. Existing
signatures made with the old private key stop counting immediately.

## Rotate Cosign artifact trust

The current trust-policy schema accepts one public key, or one exact keyless
certificate identity and issuer. It cannot express two signing keys or two
keyless identities at the same time, and agents load the policy only at startup.

For public-key mode, sign the next digest with the new key and prove that a
canary agent using a staged new policy can prepare it before changing the rest
of the fleet. Move agents in controlled pools to the new policy, then release
only artifacts accepted by the pool they target. Do not replace the sole key
fleet-wide before compatible artifacts are available.

For keyless mode, treat an identity or issuer change the same way: validate a
canary pool first, then perform a controlled rolling cutover. Do not weaken
identity or issuer constraints to create an overlap.

After each agent restart, deploy only a digest-pinned, newly signed test
artifact and confirm verification provenance in deployment status and audit
records. Roll back the policy file and restart the canary if validation fails.

## Emergency compromise response

1. Stop new `deploy` and `promote` operations. Preserve audit and deployment
   state before making changes. Rollback remains authorization-protected but
   does not require rollout approval.
2. Revoke the credential at its issuer and replace it immediately. VEKTOR has
   no CRL or OCSP configuration surface today, so do not assume a running agent
   will learn that a still-valid leaf was revoked.
3. For a compromised client leaf, remove or replace its authorization identity.
   If the issuer cannot make revocation effective at the agent, rotate the
   client CA trust bundle and restart agents.
4. For a compromised server leaf, rotate that agent's certificate and key and
   restart it. Rotate the server CA if its signing key may be compromised.
5. For a compromised approver key, remove its public key, replace approvals,
   and verify the remaining threshold is satisfiable before resuming.
6. For a compromised Cosign key, stop accepting releases under the old trust
   policy, issue a new key, re-sign approved digests, and roll out the new
   policy. Review all deployments accepted since the earliest possible
   compromise time.
7. Verify every agent and inspect audit records for unexpected identities,
   authorization failures, approvals, and artifact signers before reopening
   deployment operations.

## Rollback and completion checklist

For a routine rotation, keep the immediately previous certificate, key, and CA
bundle available in encrypted recovery storage until the new material passes
fleet-wide validation. To roll back, restore the complete matching set
atomically and restart every affected agent. Never restore compromised material.

A rotation is complete only when:

- every selected robot is reachable and healthy from a new fleet process;
- expected operations pass and forbidden operations still fail;
- audit records show only expected mTLS identities and artifact signers;
- no agent still depends on the retiring CA, identity, or public key;
- old private keys are revoked or destroyed under the organization's policy;
- monitoring and certificate-expiry alerts point at the new material; and
- the change record contains serial numbers, fingerprints, scope, timing,
  validation results, and the operator who performed the rotation.

General background: [gRPC authentication](https://grpc.io/docs/guides/auth/),
[OpenSSL certificate verification](https://docs.openssl.org/3.4/man3/X509_verify_cert/),
and [NIST SP 800-57 Part 2](https://csrc.nist.gov/pubs/sp/800/57/pt2/r1/final).
