# STAGE D7.9-R3: Audit 2 — Global Quarantine Synchronization and Race Interleavings

**Target:** Dynabook Satellite Pro C40-J (`PYS46G-06J02D`)  
**Peripheral Device:** FocalTech FT9362 / FW9369 (`spi-FTE3600:00`)  
**Host Controller:** Intel LPSS SPI (`pxa2xx-spi.2` / `idma64`)  
**Kernel:** Linux `7.2.8-9-omarchy` (x86_64 SMP PREEMPT_DYNAMIC)  
**Audit Scope:** Adversarial Synchronization Audit of Module-Global Quarantine Domain, Lock Domains, and Admission Races  

---

## 1. Executive Summary

Stage D7.9-R2 proposed a "Module-Global Bus Quarantine Domain" published under `data->lock`. This adversarial audit proves that:
1. `data->lock` (`struct mutex`) is allocated dynamically per device instance in `fte3600_probe()` (`kernel/fte3600.c:867`). **It is strictly a per-device mutex, NOT a global lock.**
2. A per-device lock cannot synchronize global state across probe, across multiple open instances, or across multiple peripheral devices on the same SPI controller.
3. If quarantine lookup and publication rely on `data->lock`, severe concurrency race conditions exist where a second transaction or a freshly probed device is admitted into a quarantined bus domain.
4. **Mandated Remediation:** The driver must implement a genuinely global synchronization primitive:
   ```c
   static DEFINE_SPINLOCK(fte3600_quarantine_lock);
   static unsigned long fte3600_quarantine_mask = 0;
   ```
   Both quarantine publication (on timeout) and quarantine lookup (in both `fte3600_probe()` and `fte3600_submit_xfer()`) must be atomic under `fte3600_quarantine_lock`.

---

## 2. Lock Domain Discrepancy Analysis

### Question 1: Does quarantine publication use a genuinely global lock or atomic primitive?
- In the proposed D7.9-R2 architecture text, publication was described as occurring under `data->lock`.
- In `kernel/fte3600.c`:
  ```c
  struct fte3600_data {
      struct spi_device *spi;
      struct mutex lock;  /* <--- PER-INSTANCE MUTEX */
      ...
  };
  ```
  This is allocated via `devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL)`.
- **Verdict:** Storing or checking global quarantine under `data->lock` is fundamentally flawed because each device instance has an independent mutex.

### Question 2: Is `data->lock` per-device or shared by all instances?
- It is strictly per-device. If multiple instances of `fte3600` exist, each has its own independent `data->lock`.

### Question 3: Can multiple peripheral devices target the same controller?
- Yes. An SPI controller supports multiple chip selects (`cs = 0, 1, ...`). Each chip select instantiates a distinct `struct spi_device`, which creates a distinct `struct fte3600_data` with its own `data->lock`.

### Question 4: Can two callers submit transactions concurrently before quarantine publication?
- On the same file descriptor / device, `data->lock` serializes ioctls.
- However, if `data->lock` is dropped during the 250 ms wait (e.g. to allow watchdog cancellation or concurrent poll/read status), or if two distinct devices share the same SPI bus, two callers can submit concurrent transactions before quarantine is published.

### Question 5: Do probe and submission use the same synchronization domain?
- **NO.** In `fte3600_probe(struct spi_device *spi)`, `data` is allocated afresh. `probe()` does not hold any existing device's `data->lock`.
- If a device is unbound and immediately rebound (`echo ... > bind`), `probe()` runs on a completely un-synchronized thread relative to an in-flight timeout on the old device.

### Question 6: Are quarantine lookup and publication atomic with respect to probe and transaction admission?
- Under D7.9-R2's proposal (using `data->lock`), they are **NOT ATOMIC**.
- Under the mandated remediation (using `fte3600_quarantine_lock`), lookup and flag check must occur inside the critical section where admission is decided.

---

## 3. Concrete Race Interleaving Diagrams

### Race 1: Unsynchronized Timeout Publication vs. Concurrent Submission on Second Instance

Assume two sensor interfaces or multiple chip-selects exist on Controller `spi1` (e.g. `cs=0` and `cs=1`), or device unbind/rebind occurs concurrently.

```
CPU 0 (Device A: Timeout Path)            CPU 1 (Device B: Admission Path)
---------------------------------------    -----------------------------------------
1. wait_for_completion_timeout() expires   
2. acquire mutex_lock(&data_A->lock)      
3. Evaluate: transaction timed out         
                                           4. Thread B calls ioctl(FTE3600_IOC_CALIB_SAMPLE)
                                           5. acquire mutex_lock(&data_B->lock)
                                           6. Check fte3600_is_quarantined(bus=1) -> FALSE!
                                           7. Issue spi_async(spi_B, msg_B) [ADMITTED!]
8. Set global quarantine: bus 1 locked    
9. release mutex_unlock(&data_A->lock)     
                                           10. release mutex_unlock(&data_B->lock)
------------------------------------------------------------------------------------
RESULT: CRITICAL SAFETY FAILURE!
A second transaction was admitted to Controller spi1 WHILE an in-flight transaction
was hung, completely violating the single-transaction quarantine invariant!
```

**Remediation:** Admission check and quarantine publication must synchronize on `fte3600_quarantine_lock`:

```
CPU 0 (Device A: Timeout Path)            CPU 1 (Device B: Admission Path)
---------------------------------------    -----------------------------------------
1. wait_for_completion_timeout() expires
2. spin_lock(&fte3600_quarantine_lock)
3. fte3600_quarantine_mask |= BIT(bus)
4. spin_unlock(&fte3600_quarantine_lock)
                                           5. spin_lock(&fte3600_quarantine_lock)
                                           6. Check mask & BIT(bus) -> TRUE!
                                           7. spin_unlock(&fte3600_quarantine_lock)
                                           8. Reject with -EBUSY! [ADMISSION BLOCKED]
```

---

### Race 2: Driver Unbind/Rebind vs. Timeout Publication

```
CPU 0 (Submitter Wait Thread)              CPU 1 (Sysfs Driver Rebind: Probe Path)
---------------------------------------    -----------------------------------------
1. wait_for_completion_timeout() (250ms)   
                                           2. echo spi-FTE3600:00 > unbind
                                           3. fte3600_spi_remove() executes
                                           4. echo spi-FTE3600:00 > bind
                                           5. fte3600_probe() starts
                                           6. Allocates new fte3600_data (new mutex!)
7. Timeout triggers                        7. Probe checks quarantine status
                                              (Unsynchronized read sees 0)
                                           8. Probe succeeds! /dev/fte3600 re-created!
                                           9. Userspace opens /dev/fte3600
8. Submitter acquires data_old->lock       10. Userspace submits new SPI transaction!
9. Writes quarantine flag                  
------------------------------------------------------------------------------------
RESULT: RACE CONDITION!
Device reprobed and admitted new transactions before old thread published quarantine.
```

**Remediation:** `fte3600_probe()` must acquire `fte3600_quarantine_lock` and verify that the target physical bus is not quarantined. If quarantined, `probe()` returns `-EBUSY` immediately, preventing character device creation.

---

### Race 3: Controller Removal & Re-Registration vs. Quarantine Publication

```
CPU 0 (Submitter Thread)                   CPU 1 (Host Controller Removal & Probe)
---------------------------------------    -----------------------------------------
1. Transaction timed out                   
                                           2. pxa2xx_spi_remove() executes
                                           3. spi_unregister_controller(ctlr1)
                                           4. idr_remove(&spi_controller_idr, bus=1)
                                           5. Hotplug/Re-probe registers new controller
                                           6. IDR assigns lowest available ID: bus=1!
                                           7. New controller is now active at bus=1
7. Submitter wakes up                      
8. Writes quarantine for (bus=1, cs=0)     
------------------------------------------------------------------------------------
RESULT: ABA TOPOLOGICAL AMBIGUITY!
Quarantine published against bus=1 targets the newly registered controller rather
than the hardware domain where the hang occurred, UNLESS the controller's permanent
hardware ACPI/PCI address (e.g. 0000:00:1e.3) is bound to the quarantine entry!
```

---

## 4. Mandated Global Synchronization Architecture

To eliminate all three race interleavings, Stage D8 source implementation must adhere to the following strict synchronization domain rules:

```c
/* Module-Global Quarantine Domain (kernel/fte3600_core.c) */
static DEFINE_SPINLOCK(fte3600_quarantine_lock);

struct fte3600_quarantine_entry {
    int bus_num;
    u16 chip_select;
    char parent_name[32]; /* e.g. "0000:00:1e.3" */
    bool active;
};

#define FTE3600_MAX_QUARANTINE_ENTRIES 8
static struct fte3600_quarantine_entry fte3600_quarantines[FTE3600_MAX_QUARANTINE_ENTRIES];

/* Atomically check and admit */
static int fte3600_quarantine_check_and_admit(struct spi_device *spi)
{
    unsigned long flags;
    int i, ret = 0;

    spin_lock_irqsave(&fte3600_quarantine_lock, flags);
    for (i = 0; i < FTE3600_MAX_QUARANTINE_ENTRIES; i++) {
        if (fte3600_quarantines[i].active &&
            fte3600_quarantines[i].bus_num == spi->controller->bus_num &&
            fte3600_quarantines[i].chip_select == spi->chip_select &&
            strcmp(fte3600_quarantines[i].parent_name, dev_name(spi->controller->dev.parent)) == 0) {
            ret = -EBUSY;
            break;
        }
    }
    spin_unlock_irqrestore(&fte3600_quarantine_lock, flags);
    return ret;
}

/* Atomically publish quarantine */
static void fte3600_quarantine_publish(struct spi_device *spi)
{
    unsigned long flags;
    int i;

    spin_lock_irqsave(&fte3600_quarantine_lock, flags);
    for (i = 0; i < FTE3600_MAX_QUARANTINE_ENTRIES; i++) {
        if (!fte3600_quarantines[i].active) {
            fte3600_quarantines[i].bus_num = spi->controller->bus_num;
            fte3600_quarantines[i].chip_select = spi->chip_select;
            strscpy(fte3600_quarantines[i].parent_name, dev_name(spi->controller->dev.parent),
                    sizeof(fte3600_quarantines[i].parent_name));
            fte3600_quarantines[i].active = true;
            break;
        }
    }
    spin_unlock_irqrestore(&fte3600_quarantine_lock, flags);
}
```

**Conclusion:** A per-device mutex cannot synchronize global quarantine. The global spinlock `fte3600_quarantine_lock` must protect both lookup and publication atomically.
