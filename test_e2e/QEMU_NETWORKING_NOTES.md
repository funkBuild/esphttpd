# QEMU Networking on ESP32-S3 — corrected status

> Status: **networking works on ESP32-S3 QEMU** as of ESP-IDF v5.5.3+.
> The original version of this file concluded the opposite; that was accurate
> when written (March) and is not any more. This revision replaces it.

## The original finding (no longer true)

The original note cited [ESP-IDF Issue #15447](https://github.com/espressif/esp-idf/issues/15447)
and concluded that OpenEth (the OpenCores Ethernet MAC) was not implemented
for ESP32-S3, making host-to-guest networking impossible. The earlier failure
manifested as:

```
qemu-system-xtensa: warning: requested NIC (anonymous, model virtio) was not created
ESP_ERROR_CHECK failed: esp_err_t 0xffffffff (ESP_FAIL) ... eth driver init
```

## What changed

Two things were needed, and both landed:

1. **The MAC**: ESP-IDF v5.5.3 added the S3 branch to the OpenEth driver —
   `components/esp_eth/src/openeth/esp_openeth.h:15-19` maps the OpenCores MAC
   to `0x600CD000` and reuses `ETS_WIFI_MAC_INTR_SOURCE` for its interrupt.
   QEMU does not emulate WiFi on this machine, so that interrupt line is free.
   (Issue #15447 described the state before this branch existed.)

2. **The PHY**: the OpenCores MAC in QEMU has no real PHY, and ESP-IDF's
   Ethernet driver requires one, so the driver needs the DP83848 PHY driver
   with a long autonegotiation timeout, exactly as IDF's own example does at
   `examples/common_components/protocol_examples_common/eth_connect.c:148-152`:

   ```c
   eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
   phy_config.autonego_timeout_ms = 100;
   eth_handle = esp_eth_phy_new_dp83848(&phy_config);
   ```

   The earlier attempts failed because they used dummy/no PHY drivers.

## Verified on this tree

The OPC UA component's e2e work verified the full path live:

- link up (`eth` netif registered, state `ESP_NETIF_LINK_UP`)
- DHCP: guest obtains `10.0.2.15` from QEMU's user-mode networking
- inbound: host → guest through `hostfwd` forwards, echoed byte-exact
- outbound: guest → host confirmed

## How to launch it

The working recipe (as used by `run_e2e_server.sh`):

```
qemu-system-xtensa -M esp32s3 \
  -drive file=flash.bin,if=mtd,format=raw \
  -nic user,model=open_eth,hostfwd=tcp::<hostport>-:<guestport>
```

Prerequisites:

- ESP-IDF **v5.5.3 or newer**
- An Espressif QEMU build that offers the `open_eth` NIC for `-machine esp32s3`
  (`qemu-system-xtensa -machine esp32s3 -net nic,model=help` lists it;
  `esp_develop_9.2.2_20250817` or newer is known good)

## Notes

- Virtio NICs are still not supported by this machine model (`model=virtio`
  produces the "no peer / not supported" warnings above). Use `open_eth`.
- The MAC driver's init runs `esp_eth_phy_new_dp83848` with
  `autonego_timeout_ms = 100`; a shorter timeout races the emulated PHY.
