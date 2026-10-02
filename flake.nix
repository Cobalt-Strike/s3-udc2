{
  description = "dev shell for s3-udc2";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    rust-overlay.url = "github:oxalica/rust-overlay";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      rust-overlay,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs {
          inherit system overlays;
          config = {
            allowUnfreePredicate = pkg: builtins.elem (nixpkgs.lib.getName pkg) [ "terraform" ];
          };
        };
      in
      {
        devShells.default =
          with pkgs;
          mkShell {
            buildInputs = [
              rust-bin.stable.latest.default
              awscli2
              terraform
              pkgsCross.mingwW64.buildPackages.gcc
              pkgsCross.mingw32.buildPackages.gcc
              mermaid-cli
            ];
            shellHook = ''
              # Keep Nix tools ahead of rustup shims for Cargo subcommands.
              export PATH="$PATH:''${CARGO_HOME:-$HOME/.cargo}/bin"
            '';
          };
      }
    );
}
