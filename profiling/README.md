# Profiling builds

ELF and map files of the exact builds that produced profiler samples
(`romm/prof-*.bin` on the SD card). Decode with:

```bash
gunzip -k profiling/0.8.0/*.gz
tools/prof_report.py prof-net.bin profiling/0.8.0/romm-poc-rx128.elf profiling/0.8.0/romm-poc-rx128.map
```
