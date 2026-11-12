# DockStation Linux diagnostic gateway — first VM slice

This is a temporary, read-only connectivity prototype for the Windows operator client.
It uses Python 3 standard-library modules only; there are no third-party Python
runtime dependencies.

## Run

From the repository root on the Linux VM:

```sh
python3 -m repka_gateway.dockstation_gateway.main --port 8080
```

Optionally enable a system-name-resolution check:

```sh
DOCKSTATION_DNS_CHECK_NAME=example.com \
  python3 -m repka_gateway.dockstation_gateway.main --port 8080
```

The service binds **only** to `127.0.0.1`. It does not listen on the VM's
Ethernet address, alter firewall/network configuration, enable SSH, or perform
physical station actions. The intended Windows path is an SSH local-forward
from the Windows host to `127.0.0.1:8080` inside the VM.

## API

- `GET http://127.0.0.1:8080/api/v1/health`
- `GET http://127.0.0.1:8080/api/v1/system/diagnostics`

By default, all other routes return 404 and non-GET methods return 405. Responses are JSON
with `Cache-Control: no-store`. The diagnostics object includes a UTC collection
time, schema version, system platform details, network interfaces and addresses,
default routes, optional DNS resolution, SSH unit state, and an aggregate status.

A separate test-only route can be enabled for VM integration checks:

```sh
python3 -m repka_gateway.dockstation_gateway.main --port 8080 --enable-test-adapter
```

When explicitly enabled, `POST /api/v1/test/adapter/request` accepts only a schema-1.0 `PING` request and passes it through TestRequestService to TestRequestAdapter. The receipt is marked `TEST_ADAPTER_ACCEPTED`, `source=TEST_ADAPTER`, and `physical_action_performed=false`. The body is limited to 4096 bytes. Unknown fields, duplicate JSON keys and any operation other than PING are rejected. The route is disabled by default; it is not a production command API and performs no physical action.

The health route means only that the gateway process is responding. It is not a
claim that the Ethernet route, DNS, SSH, or physical station is healthy.

## Scope and limitations

- This prototype reports the Linux VM/host, not ESP32 data or physical sensors.
- DNS uses the operating system resolver (`getent ahosts`); `/etc/hosts` can
  satisfy the lookup. An unset DNS target is reported as `NOT_CONFIGURED`.
- SSH reporting reads systemd unit state. It does not authenticate to SSH or
  prove that a Windows key-based login succeeds.
- The API has no separate application login. It is loopback-only and intended
  to be reached through SSH with strict host-key verification. Do not expose it
  on a LAN or public interface.
- Python is a temporary implementation choice for this minimum VM slice to
  avoid choosing an unapproved HTTP/JSON dependency. It does not settle the
  longer-term C++17 Gateway implementation described in the master plan.
