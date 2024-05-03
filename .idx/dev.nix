# To learn more about how to use Nix to configure your environment
# see: https://developers.google.com/idx/guides/customize-idx-env
{ pkgs, ... }: {
  # Which nixpkgs channel to use.
  channel = "stable-23.11"; # or "unstable"
  # Use https://search.nixos.org/packages to find packages
  packages = with pkgs; [
    act
    autoconf
    clang
    clang-tools
    ed
    fira-code-nerdfont
    gcc
    gdb
    gettext
    glibc.out
    glibc.static
    gnumake
    graphviz-nox
    libbacktrace
    libtool
    lldb
    m4
    neovim
    perl
    pkg-config
    python3
    ripgrep
    # pkgs.python311
    # pkgs.python311Packages.pip
  ];
  # Sets environment variables in the workspace
  env = { };
  idx = {
    # Search for the extensions you want on https://open-vsx.org/ and use "publisher.id"
    extensions = [
      "asvetliakov.vscode-neovim"
      "coolbear.systemd-unit-file"
      "dotjoshjohnson.xml"
      "editorconfig.editorconfig"
      "esbenp.prettier-vscode"
      "golang.go"
      "mads-hartmann.bash-ide-vscode"
      "ms-python.python"
      "ms-python.vscode-pylance"
      "ms-vscode.clangd"
      "ms-vscode.cmake-tools"
      "ms-vscode.cpptools"
      "ms-vscode.cpptools-extension-pack"
      "ms-vscode.makefile-tools"
      "ms-vsliveshare.vsliveshare"
      "mspython.debugpy"
      "redhat.vscode-yaml"
      "rogalmic.bash-debug"
      "ryu1kn.partial-diff"
      "scala-lang.scala"
      "scalameta.metals"
      "streetsidesoftware.code-spell-checker"
      "timonwong.shellcheck"
      "twxs.cmake"
      "vadimcn.vscode-lldb"
      "vscode-icons-team.vscode-icons"
      "vscodevim.vim"
      "yzhang.markdown-all-in-one"
      "znck.grammarly"
      #"jnoortheen.nix-ide"
    ];
    # Enable previews
    previews = {
      enable = true;
      previews = {
        # web = {
        #   # Example: run "npm run dev" with PORT set to IDX's defined port for previews,
        #   # and show it in IDX's web preview panel
        #   command = ["npm" "run" "dev"];
        #   manager = "web";
        #   env = {
        #     # Environment variables to set for your server
        #     PORT = "$PORT";
        #   };
        # };
      };
    };
    # Workspace lifecycle hooks
    workspace = {
      # Runs when a workspace is first created
      onCreate = {
        # Example: install JS dependencies from NPM
        # npm-install = 'npm install';
      };
      onStart = {
        # Example: start a background task to watch and re-build backend code
        # watch-backend = "npm run watch-backend";
      };
    };
  };
}
