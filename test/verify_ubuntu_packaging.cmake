set(packaging_root "${CMAKE_CURRENT_LIST_DIR}/../packaging/ubuntu")
set(debian_root "${CMAKE_CURRENT_LIST_DIR}/../debian")

foreach(file
    vektor-agent.service
    agent.env
    policy.yaml
    trust.yaml
    authorization.yaml)
  if(NOT EXISTS "${packaging_root}/${file}")
    message(FATAL_ERROR "missing Ubuntu packaging file: ${file}")
  endif()
endforeach()

file(READ "${packaging_root}/vektor-agent.service" service)
foreach(required
    "User=vektor"
    "Group=vektor"
    "ConditionPathExists=/etc/vektor/policy.yaml"
    "--config /etc/vektor/policy.yaml"
    "--deployment-state /var/lib/vektor/deployment.yaml"
    "--audit-log /var/log/vektor/audit.jsonl"
    "--tls-key /etc/vektor/tls/agent.key"
    "NoNewPrivileges=yes"
    "ProtectSystem=strict"
    "ReadWritePaths=/var/lib/vektor /var/log/vektor"
    "WantedBy=multi-user.target")
  string(FIND "${service}" "${required}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "service is missing required setting: ${required}")
  endif()
endforeach()

file(READ "${debian_root}/vektor.postinst" postinst)
foreach(required "adduser --system" "install -d -o vektor" "chmod 0640")
  string(FIND "${postinst}" "${required}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "postinst is missing required operation: ${required}")
  endif()
endforeach()

foreach(file control changelog copyright rules vektor.install vektor.conffiles)
  if(NOT EXISTS "${debian_root}/${file}")
    message(FATAL_ERROR "missing Debian packaging file: ${file}")
  endif()
endforeach()
