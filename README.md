# asus-wmi-screenpad-module

A kernel module adding an LED class device for the screenpad on some ASUS Zenbooks

This module was created using the [asus-wmi-screenpad](https://github.com/Plippo/asus-wmi-screenpad) patches, but distributed as a separate module to make installation easier and prevent breakages

## Installation

This is meant to be used with NixOS

1. Add this repo to your flake's inputs
2. Override the kernel parameter with something liike

```nix
asus-wmi-screenpad = inputs.asus-wmi-screenpad.defaultPackage.${system}.override kernelPackages.kernel.dev;
```

3. Add the new package to your `boot.extraModulePackages`
4. Optionally enable it by default by adding `asus-wmi-screenpad` to `boot.kernelModules`

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
