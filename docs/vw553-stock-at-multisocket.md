# VW553 stock AT multi-socket validation

## Decision

The H77 adapter uses the vendor stock `CIPMUX=1` command set for outbound
client sockets. CAP2 is optional and is no longer required to create multiple
Tuya socket descriptors.

The adapter maps three identities explicitly:

```text
Tuya logical fd  <->  CIPSTART con_id  <->  +IPD/CIPSTATE lwip fd
```

`+IPD` reports the lwip fd, not the `CIPSTART` con_id. `AT+CIPSTATE?` is queried
after connect to establish the mapping.

## Minimal VW553 requirement

Stock firmware sends an IPD header, payload, CRLF and trailing OK through
separate UART writes. Concurrent receive traffic can interleave these writes.
The required VW553 change is therefore limited to serializing the complete IPD
frame under the existing UART TX semaphore. Accept URCs, dual listeners and
BLE GATT extensions are separate future work.

Validated minimal image:

```text
.cache/gino-validation/vw553-ipd-candidate-20261019/image-all.bin
sha256 f284c1d423997ab061943f8853b38a7a923ed7f6689e8d8b10e47bab37c82dd4
```

## Hardware results

- Vendor stock AT with four concurrent TCP clients returned `0,OK` through
  `3,OK`; `+IPD` was observed for all four lwip fds.
- H77 `http_client` in stock mode returned 200 for `httpbin.org` and
  `www.baidu.com` sequentially with no transport poisoning.
- H77 `net_multi_http httpbin.org httpbin.org` kept two TCP sockets connected
  simultaneously, sent both requests, and received HTTP 200 on both.
- Repeating the concurrent test succeeded, including `CIPCLOSE=<id>` followed
  by reuse of the same con_ids.
- A stock image without the atomic-IPD patch failed the concurrent receive
  test with a bad separator and transport poisoning, confirming the need for
  the minimal 553 patch.

## Still pending for AP/BLE provisioning

- TCP accept URC with con_id/client fd/peer information
- one concurrent TCP listener plus one UDP listener
- dynamic BLE GATT/service event extension

These should be added as a small capability-gated extension, not by reviving
the CAP2 `STATE_QUERY`/`RETIRED`/`CLOSE_ACK` state machine.
