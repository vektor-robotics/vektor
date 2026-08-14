#!/usr/bin/env bash
set -euo pipefail

registry="${VEKTOR_TEST_REGISTRY:-registry:5000}"
output_file="${1:-/tmp/vektor-trust-integration.env}"
fixture_dir="$(mktemp -d)"
trap 'rm -rf "${fixture_dir}"' EXIT

for attempt in $(seq 1 30); do
  if curl --fail --silent "http://${registry}/v2/" >/dev/null; then
    break
  fi
  if [[ "${attempt}" == "30" ]]; then
    echo "OCI registry did not become ready at ${registry}" >&2
    exit 1
  fi
  sleep 1
done

push_artifact() {
  local repository="$1"
  local tag="$2"
  local marker="$3"
  local config_path="${fixture_dir}/${repository//\//_}-${marker}.json"
  local manifest_path="${config_path}.manifest"

  printf '{"architecture":"amd64","os":"linux","config":{"Labels":{"org.vektor.fixture":"%s"}},"rootfs":{"type":"layers","diff_ids":[]}}' \
    "${marker}" >"${config_path}"
  local config_digest="sha256:$(sha256sum "${config_path}" | cut -d' ' -f1)"
  local config_size
  config_size="$(wc -c <"${config_path}" | tr -d ' ')"

  curl --fail --silent --show-error -X POST \
    -H 'Content-Type: application/octet-stream' \
    --data-binary "@${config_path}" \
    "http://${registry}/v2/${repository}/blobs/uploads/?digest=${config_digest}" \
    >/dev/null

  printf '{"schemaVersion":2,"mediaType":"application/vnd.oci.image.manifest.v1+json","config":{"mediaType":"application/vnd.oci.image.config.v1+json","digest":"%s","size":%s},"layers":[]}' \
    "${config_digest}" "${config_size}" >"${manifest_path}"
  local manifest_digest="sha256:$(sha256sum "${manifest_path}" | cut -d' ' -f1)"

  curl --fail --silent --show-error -X PUT \
    -H 'Content-Type: application/vnd.oci.image.manifest.v1+json' \
    --data-binary "@${manifest_path}" \
    "http://${registry}/v2/${repository}/manifests/${tag}" >/dev/null

  printf '%s/%s@%s' "${registry}" "${repository}" "${manifest_digest}"
}

signed_digest="$(push_artifact vektor/signed v1 signed)"
unsigned_digest="$(push_artifact vektor/unsigned v1 unsigned)"
tampered_original="$(push_artifact vektor/tampered latest original)"

export COSIGN_PASSWORD='vektor-integration-test'
cosign generate-key-pair --output-key-prefix="${fixture_dir}/trusted" >/dev/null
cosign generate-key-pair --output-key-prefix="${fixture_dir}/untrusted" >/dev/null
cosign sign --yes --key "${fixture_dir}/trusted.key" \
  --allow-http-registry --tlog-upload=false "${signed_digest}" >/dev/null
cosign sign --yes --key "${fixture_dir}/trusted.key" \
  --allow-http-registry --tlog-upload=false "${tampered_original}" >/dev/null

# Move the signed tag to different content. Verification of the tag must now fail.
push_artifact vektor/tampered latest replacement >/dev/null

install -d "$(dirname "${output_file}")/vektor-trust-fixtures"
key_dir="$(cd "$(dirname "${output_file}")/vektor-trust-fixtures" && pwd)"
install -m 0644 "${fixture_dir}/trusted.pub" "${key_dir}/trusted.pub"
install -m 0644 "${fixture_dir}/untrusted.pub" "${key_dir}/untrusted.pub"

cat >"${output_file}" <<EOF
export VEKTOR_COSIGN_BIN='$(command -v cosign)'
export VEKTOR_SIGNED_ARTIFACT='${signed_digest}'
export VEKTOR_UNSIGNED_ARTIFACT='${unsigned_digest}'
export VEKTOR_TAMPERED_ARTIFACT='${registry}/vektor/tampered:latest'
export VEKTOR_TRUSTED_PUBLIC_KEY='${key_dir}/trusted.pub'
export VEKTOR_UNTRUSTED_PUBLIC_KEY='${key_dir}/untrusted.pub'
EOF

echo "Prepared real Cosign trust fixtures in ${output_file}"
