# ACHEM

ACHEM (A Channel Emulator) is an open source, real time digital twin framework for USRP based wireless systems. It combines **V-USRP** (virtual USRP, radio emulation) and **CHEM** (I/Q level channel emulation) to enable unmodified UHD based SDR applications to run in a fully software based environment while preserving key timing and channel behaviors.

## What is in this site

1. **[Getting Started](getting%2Dstarted/installation.md)**: build and install basics and a minimal configuration walkthrough.
2. **[Overview](overview.md)**: overall system design, component overview, and signal model.
3. **[V-USRP](vusrp.md)**: Virtual USRP. Signal frames and transmit and receive processing.
4. **[CHEM](chem.md)**: Channel emulator internals. node connection, channel intermediates, IQ data flow, and configuration.
5. **[PyCHEM](pychem.md)**: the Python management interface for scripting and orchestration.
6. **[Channel Models](channel%2Dmodels.md)**: propagation models and their equations (FSPL, Two Ray, 3GPP TR 38.901, Okumura Hata, Longley Rice).
7. **[Extensions](extensions.md)**: pluggable channel modeling backends (Sionna RT, custom extensions).
8. **[Benchmark](benchmark.md)**: performance testing tool to measure system capacity.
9. **[API Reference](api%2Dreference.md)**: Doxygen generated API documentation (In progress).

## Quick start

1. Follow the installation steps in [Getting Started: Installation](getting%2Dstarted/installation.md).
2. Review [Getting Started: Configuration](getting%2Dstarted/configuration.md) to understand the config file.
3. Read the [Overview](overview.md) page for a high level overview of how ACHEM works.
4. If you are using Python workflows, start with [PyCHEM](pychem.md).
5. Review the available [Channel Models](channel%2Dmodels.md) for propagation modeling.
6. To integrate external channel or RF impairment models, see [Extensions](extensions.md).
7. To measure your system's capacity, see [Benchmark](benchmark.md).

## Publication

If you use ACHEM in your research, please cite:

```bibtex
@article{gurses2026achem,
  title     = {{ACHEM}: A Real Time Digital Twin Framework with Channel and Radio Emulation},
  author    = {G{\"u}rses, An{\i}l and Sichitiu, Mihail L.},
  journal   = {arXiv preprint arXiv:2604.04742},
  year      = {2026}
}
```

The full paper is available [here](https://arxiv.org/abs/2604.04742).
