# asus-wmi-screenpad-module

A kernel module adding an LED class device for the screenpad on some ASUS Zenbooks

This module was created using the [asus-wmi-screenpad](https://github.com/Plippo/asus-wmi-screenpad) patches, but distributed as a separate module to make installation easier and prevent breakages

## NixOS Installation

1. Add this repo to your flake's inputs
2. Override the kernel parameter with something like this

```nix
asus-wmi-screenpad = inputs.asus-wmi-screenpad.defaultPackage.${system}.override kernelPackages.kernel;
```

3. Add the new package to your `boot.extraModulePackages`
4. Optionally enable it by default by adding `asus-wmi-screenpad` to `boot.kernelModules`

## DKMS Installation (for non-NixOS)

This project is meant to be used with NixOS, but it *should* work with DKMS

1. Add the repo to DKMS with `sudo dkms add .`
2. Build and install the module with `sudo dkms install asus-wmi-screenpad/0.1`
3. Load the module with `sudo modprobe asus-wmi-screenpad`

## Building manually (with Nix)

To enter a shell with the dependencies to develop run

```nix
nix develop --override-input nixpkgs nixpkgs
```

`-ovveride-input` is used to make sure that `nixpkgs` is matched with your system's `nixpkgs`

Use the build script to provide a custom kernel

```nix
nix run --override-input nixpkgs nixpkgs path:.#buildWithKernel linuxPackages_latest
````

## Configuration

There are two command-line module parameters

- `enable_led_dev` (default `=true`) Registers an LED device
- `enable_bl_dev` (default `=false`) Registers a backlight device
