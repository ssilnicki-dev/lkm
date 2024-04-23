# lkm

The example module implements a driver for an imaginary device that emits an interrupt when it is ready for data acquisition.

The frequency of interrupt emission can be adjusted in microseconds using the kt_period_param_us module parameter (default value = 1,000,000 us).
The bus (or perhaps the device itself; this is unclear) used to access the device, experiences operational jitter, which may delay access to the device within a predictable range, specified by the device_read_jitter_range_param_us parameter (default = 25 us).
According to the datasheet, the device requires at least 80 us to transfer data from its internal buffers.
Additionally, the device itself exhibits a jitter in interrupt emission, controlled by the kt_period_jitter_range_param_us parameter (default = 100 us).

During normal operations, the driver receives interrupts from the device cyclically (simulated by a repeated high-resolution kernel timer) and reads data from the device in separate work to avoid blocking in the interrupt context. Due to inconsistencies between the device and settings, the driver may skip reading data from the device when it detects that a recent read attempt has not yet completed. For both successful readings and omissions, it collects statistics, which can be accessed via a sysfs-exposed endpoint (/sys/kernel/lkm_data/stat).

The driver also exposes a separate sysfs endpoint to read a "magic value" from the driver, which is calculated based on the most recent data received from the device (/sys/kernel/lkm_data/current_data).
