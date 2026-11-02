# Installing Common

Common is one C file with no dependencies. On every platform the steps are:
get a C compiler, build `src/cl.c`, put the result on your PATH.

There are no binary downloads yet. Building takes about one second.

What has been verified for 0.4.0: macOS (Apple clang, Apple Silicon), and
Linux arm64 (Ubuntu 24.04, gcc 13) with and without SQLite and Postgres,
build and pass the full test suite, including under AddressSanitizer, UBSan
and, on Linux, the leak checker. Earlier, for 0.3.0, Linux x86-64 (gcc 13,
static build with SQLite) passed too, and the Windows native build
cross-compiled with MinGW. For 0.4.0 the Windows build cross-compiles with MinGW with no
warnings, but it has not yet been run on a Windows machine. If you try it,
please open an issue either way.

---

## macOS

1. Install the compiler if you do not have it. This opens an Apple dialog, click Install:

   ```sh
   xcode-select --install
   ```

2. Build and install:

   ```sh
   git clone https://github.com/commonlang/cl.git
   cd cl
   make
   sudo make install      # copies cl to /usr/local/bin
   ```

3. Check:

   ```sh
   cl words
   cl serve 8000 examples
   ```

   Then open http://localhost:8000/hello.

Tested on Apple Silicon. Intel Macs use the same steps but have not been tested yet. Homebrew is not needed.

---

## Linux

1. Install a compiler and make:

   ```sh
   # Debian, Ubuntu, Mint
   sudo apt install build-essential git

   # Fedora
   sudo dnf install gcc make git

   # Arch
   sudo pacman -S base-devel git

   # Alpine
   apk add build-base git
   ```

2. Build and install:

   ```sh
   git clone https://github.com/commonlang/cl.git
   cd cl
   make
   sudo make install
   ```

   To install for just your user instead, with no sudo:

   ```sh
   make install PREFIX=$HOME/.local
   ```

   and make sure `~/.local/bin` is on your PATH.

3. Check with `cl words`, then `cl serve 8000 examples`.

---

## Windows

Two ways. The first is the easiest and gives you the exact Linux experience.

### Option A: WSL (recommended)

1. Open PowerShell as administrator and run `wsl --install`, then restart.
2. Open the Ubuntu app from the Start menu and follow the Linux steps above.
3. In the Windows browser, http://localhost:8000/ reaches the server running inside WSL.

### Option B: native build with MinGW

1. Install a compiler. The simplest is [w64devkit](https://github.com/skeeto/w64devkit):
   download the zip, unzip it anywhere, run `w64devkit.exe` to get a shell
   with `gcc` and `make`.

2. Build:

   ```sh
   git clone https://github.com/commonlang/cl.git
   cd cl
   gcc -O2 -Wall -Wextra -o common.exe src/cl.c -lws2_32
   ```

3. Copy `common.exe` to a folder on your PATH, or run it from the repo:

   ```sh
   common.exe words
   common.exe serve 8000 examples
   ```

   Note the name. On Windows the binary is `common.exe`, not `cl.exe`, because
   `cl.exe` is Microsoft's C compiler and is already on the PATH on any machine
   with Visual Studio. Everything else is the same.

Visual Studio's own compiler also works, from a Developer Command Prompt:

```
cl /O2 /W3 /Fe:common.exe src\cl.c
```

(`ws2_32.lib` is linked automatically by a pragma in the source.)

The test suite is a shell script, so on Windows run it from WSL or from the
w64devkit shell: `sh tests/run.sh`.

---

## Building by hand without make

Any C11 compiler works:

```sh
cc -O2 -o cl src/cl.c                                          # macOS, Linux, BSD: no database words
cc -O2 -DCL_SQLITE -o cl src/cl.c src/mod_sqlite.c -lsqlite3    # the same, with SQLite
gcc -O2 -o common.exe src/cl.c -lws2_32                        # Windows, MinGW
```

Each database is a module in `src/`: `mod_sqlite.c` needs `-DCL_SQLITE` and
`-lsqlite3`, `mod_postgres.c` needs `-DCL_POSTGRES` and `-lpq`. `make` works
this out from what is installed.

---

## Checking the build

```sh
make test        # the test suite
make sanitize    # the same suite under AddressSanitizer and UBSan (clang or gcc)
```

The tests also use `curl`, `python3`, `pkill` and, when `cl` has SQLite, the `sqlite3` command. macOS has them all. Debian/Ubuntu: `apt install curl python3 procps sqlite3`.

---

## Uninstalling

```sh
sudo rm /usr/local/bin/cl
```
