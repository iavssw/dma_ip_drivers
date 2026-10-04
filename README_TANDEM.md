# V80 QDMA Tandem PCIe Setup

Build and install the QDMA Linux driver with Tandem boot support, then transfer a
PDI to the slave boot interface (SBI) over PCIe.

The examples use PCI address `0000:01:00.0`, QDMA device `qdma01000`, and queue 0.
Replace the PCI address and QDMA device name with the values for your system.
Have the Tandem stage 1 design loaded and the corresponding stage 2 PDI available
before following the programming steps.

## 1. Install dependencies

On Ubuntu or Debian:

```bash
sudo apt install build-essential libaio-dev "linux-headers-$(uname -r)"
```

The driver requires kernel headers or a configured kernel source tree matching
the running kernel. See the [QDMA driver README](QDMA/linux-kernel/docs/README)
for the full build requirements.

## 2. Check the V80 PCI device IDs

The V80 entries below are already included in this fork's
[PCI device table](QDMA/linux-kernel/driver/src/pci_ids.h). If using another
checkout, ensure they are present in `pci_ids[]` before building:

```c
{ PCI_DEVICE(0x10ee, 0x50b4), },        /** V80 */
{ PCI_DEVICE(0x10ee, 0x50b5), },        /** V80 */
```

## 3. Build and install

From the repository root, enter the QDMA Linux driver directory. Run the
remaining build and configuration commands from this directory:

```bash
cd QDMA/linux-kernel
make TANDEM_BOOT_SUPPORTED=1
sudo make install
```

This builds the driver and applications, then installs them in the standard
system locations.

## 4. Configure and load the driver

For the first installation on a host, create `/etc/modprobe.d/qdma.conf` manually
using the [QDMA configuration instructions](QDMA/linux-kernel/docs/README), or
use the supplied helper. Replace the placeholders with your design's settings:

```text
sudo bash scripts/qdma_generate_conf_file.sh <bus_num> <num_pfs> <mode> <config_bar> <master_pf>
```

The helper writes `/etc/modprobe.d/qdma.conf`. Use `lspci -D` to identify the PCI
address; for the example address `0000:01:00.0`, the bus argument is `0x01`.

To prevent automatic loading of the QDMA modules at boot, add these lines to
`/etc/modprobe.d/blacklist.conf` as recommended by the driver README:

```text
blacklist qdma-pf
blacklist qdma-vf
```

Load the physical-function driver and list the devices it has detected:

```bash
sudo modprobe qdma-pf
sudo dma-ctl dev list
```

Use the QDMA device name reported by `dma-ctl dev list` in the commands below.

## 5. Configure the programming queue

Allocate one queue through sysfs:

```bash
echo 1 | sudo tee /sys/bus/pci/devices/0000:01:00.0/qdma/qmax
```

Create queue 0 in memory-mapped (`mm`), host-to-card (`h2c`) mode. Start it with a
4096-byte aperture so transfers use keyhole addressing:

```bash
sudo dma-ctl qdma01000 q add idx 0 mode mm dir h2c
sudo dma-ctl qdma01000 q start idx 0 dir h2c aperture_sz 4096
```

## 6. Transfer the PDI

Set `PDI_FILE` to the stage 2 PDI. The command below uses the file's size in bytes
and sends it to the SBI address `0x102100000`:

```bash
PDI_FILE=/path/to/stage2.pdi
sudo dma-to-device \
    -d /dev/qdma01000-MM-0 \
    -f "$PDI_FILE" \
    -s "$(stat -c %s "$PDI_FILE")" \
    -a 0x102100000
```

## 7. Reload the driver for CPM4 QDMA only

For CPM4 QDMA, reload the driver after the transfer and recreate the queue
without the programming aperture:

```bash
sudo rmmod qdma-pf
sudo modprobe qdma-pf
echo 1 | sudo tee /sys/bus/pci/devices/0000:01:00.0/qdma/qmax
sudo dma-ctl qdma01000 q add idx 0 mode mm dir h2c
sudo dma-ctl qdma01000 q start idx 0 dir h2c
```

## References

- [QDMA Linux driver README](QDMA/linux-kernel/docs/README)
- [AMD PG347: Tandem PCIe design requirements](https://docs.amd.com/r/en-US/pg347-cpm-dma-bridge/Design-Requirements)
- [AMD PG347: QDMA programming procedure](https://docs.amd.com/r/en-US/pg347-cpm-dma-bridge/QDMA)
