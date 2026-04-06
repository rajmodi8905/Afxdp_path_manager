# TRex Setup for AF_XDP Traffic Generation

This guide sets up TRex on a separate machine so it can generate traffic toward the AF_XDP manager in this repository.

## 1. Start the AF_XDP manager on the DUT

Run these commands on the DUT machine, where the AF_XDP manager listens on the target interface.

```bash
sudo ip link set enp68s0f0 up
cd ~/Afxdp_path_manager/onvm/onvm_mgr
sudo ./onvm_mgr_afxdp -d enp68s0f0 -v
```

If you use a different interface, replace `enp68s0f0` with your DUT interface name.

## 2. Install TRex prerequisites on the TRex host

Run these commands on the machine that will transmit packets with TRex.

```bash
sudo apt update
sudo apt install -y build-essential python3 python3-pip pciutils ethtool numactl
```

## 3. Reserve hugepages on the TRex host

TRex uses hugepages for DPDK memory. Reserve them before starting TRex.

```bash
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
```

If you want to verify the current hugepage state, use:

```bash
grep Huge /proc/meminfo
```

## 4. Download and extract TRex

Download the official TRex Linux package from the TRex release page, then extract it.

```bash
mkdir -p ~/trex
cd ~/trex
tar -xzf trex-*.tar.gz
cd v*
```

Replace `trex-*.tar.gz` with the exact archive name you downloaded.

## 5. Identify the TRex data-port PCI address

TRex must know which NIC port it will use for packet generation.

```bash
sudo ./dpdk_setup_ports.py -s
```

If your package layout differs, run the same script from the extracted TRex directory.

## 6. Check the DUT MAC address

TRex needs the destination MAC address of the DUT interface.

```bash
ip link show enp68s0f0
```

Record the `link/ether` address from the output.

## 7. Create the TRex configuration file

Create `/etc/trex_cfg.yaml` on the TRex host.

```bash
sudo tee /etc/trex_cfg.yaml >/dev/null <<'EOF'
- version: 2
  interfaces:
    - "0000:18:00.0"
  port_info:
    - dest_mac: "aa:bb:cc:dd:ee:ff"
  platform:
    master_thread_id: 0
    latency_thread_id: 1
    dual_if:
      - socket: 0
        threads: [2, 3, 4, 5]
EOF
```

Replace these placeholders before using the file:

```text
0000:18:00.0       -> the PCI address of your TRex data NIC
aa:bb:cc:dd:ee:ff  -> the MAC address of enp68s0f0 on the DUT
```

## 8. Disable offloads on the TRex data NIC

This is often required for clean packet generation and easier debugging.

```bash
sudo ip link set <trex_nic> up
sudo ethtool -K <trex_nic> gro off gso off tso off lro off
```

Replace `<trex_nic>` with the Linux interface name mapped to the TRex PCI device.

## 9. Start the TRex server

Run the TRex daemon in stateless mode.

```bash
sudo ./t-rex-64 -i --stl -c 4 --cfg /etc/trex_cfg.yaml
```

Keep this process running while you generate traffic.

## 10. Open the TRex console

Use a second terminal on the TRex host.

```bash
cd ~/trex/v*/automation/trex_control_plane/interactive
./trex-console
```

## 11. Connect to the running TRex server

Inside the TRex console, run:

```text
connect
```

## 12. Start packet generation

Use a simple UDP stream example to generate traffic.

```text
start -f stl/udp_1pkt_simple.py -m 100kpps -d 30 -p 0
```

This sends packets on port 0 at 100 kpps for 30 seconds.

## 13. Check TRex statistics

After starting the stream, inspect counters.

```text
stats
```

## 14. Verify reception on the DUT

On the AF_XDP host, confirm that packets are arriving.

```bash
sudo tcpdump -ni enp68s0f0 -c 20
```

You can also watch the AF_XDP manager logs and counters in its terminal.

## 15. Useful cleanup commands

Stop the TRex console with:

```text
quit
```

Stop the TRex server with `Ctrl+C` in the server terminal.

## Complete command list

For convenience, here is the full command sequence in order.

```bash
sudo ip link set enp68s0f0 up
cd ~/Afxdp_path_manager/onvm/onvm_mgr
sudo ./onvm_mgr_afxdp -d enp68s0f0 -v

sudo apt update
sudo apt install -y build-essential python3 python3-pip pciutils ethtool numactl
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
grep Huge /proc/meminfo

mkdir -p ~/trex
cd ~/trex
tar -xzf trex-*.tar.gz
cd v*
sudo ./dpdk_setup_ports.py -s
ip link show enp68s0f0

sudo tee /etc/trex_cfg.yaml >/dev/null <<'EOF'
- version: 2
  interfaces:
    - "0000:18:00.0"
  port_info:
    - dest_mac: "aa:bb:cc:dd:ee:ff"
  platform:
    master_thread_id: 0
    latency_thread_id: 1
    dual_if:
      - socket: 0
        threads: [2, 3, 4, 5]
EOF

sudo ip link set <trex_nic> up
sudo ethtool -K <trex_nic> gro off gso off tso off lro off
sudo ./t-rex-64 -i --stl -c 4 --cfg /etc/trex_cfg.yaml

cd ~/trex/v*/automation/trex_control_plane/interactive
./trex-console

connect
start -f stl/udp_1pkt_simple.py -m 100kpps -d 30 -p 0
stats

sudo tcpdump -ni enp68s0f0 -c 20
```

## Notes

The placeholders in this file must be replaced with your actual PCI address, MAC address, and TRex interface name.
