# SRTLA - Windows Fork

This is a Windows-compatible fork of the [SRTLA](https://github.com/BELABOX/srtla) project, which enables SRT transport proxy with link aggregation for connection bonding.

## Windows Support

This fork adds Windows compatibility through:
- WSAStartup/WSACleanup for Windows socket initialization
- Windows cryptography API for random number generation (replacing /dev/urandom)
- Platform-specific macros and compatibility code

## Building on Windows

### Prerequisites
- MinGW-w64 or Visual Studio with C/C++ tools
- Git (for version information)

### Build Steps
```batch
git clone https://github.com/abdulkadirozyurt/srtla-windows.git
cd srtla-windows
make
```

This will produce 2 executables: `srtla_send.exe` and `srtla_rec.exe`.

## Usage

### Receiver Mode (srtla_rec)
```batch
srtla_rec.exe [listen_port] [srt_host] [srt_port]
```

Example:
```batch
srtla_rec.exe 5000 127.0.0.1 5002
```

### Sender Mode (srtla_send)
```batch
srtla_send.exe [listen_port] [srtla_host] [srtla_port] [source_ips_file]
```

Example:
```batch
srtla_send.exe 5001 receiver_ip_address 5000 source_ip_list.txt
```

## Running as a Windows Service

You can use NSSM (Non-Sucking Service Manager) to run SRTLA components as Windows services.

### Installing NSSM
1. Download NSSM from [nssm.cc](https://nssm.cc/download)
2. Extract the archive to a folder on your system
3. Optional: Add the NSSM folder to your system PATH

### Creating SRTLA Receiver Service
```batch
nssm.exe install SRTLA-Receiver
nssm.exe set SRTLA-Receiver Application C:\path\to\srtla_rec.exe
nssm.exe set SRTLA-Receiver AppParameters "5000 127.0.0.1 5002"
nssm.exe set SRTLA-Receiver AppDirectory C:\path\to\srtla-windows
nssm.exe set SRTLA-Receiver DisplayName "SRTLA Receiver Service"
nssm.exe set SRTLA-Receiver Description "SRT transport proxy receiver with link aggregation"
nssm.exe set SRTLA-Receiver Start SERVICE_AUTO_START
nssm.exe start SRTLA-Receiver
```

### Creating SRTLA Sender Service
```batch
nssm.exe install SRTLA-Sender
nssm.exe set SRTLA-Sender Application C:\path\to\srtla_send.exe
nssm.exe set SRTLA-Sender AppParameters "5001 receiver_ip_address 5000 C:\path\to\source_ip_list.txt"
nssm.exe set SRTLA-Sender AppDirectory C:\path\to\srtla-windows
nssm.exe set SRTLA-Sender DisplayName "SRTLA Sender Service"
nssm.exe set SRTLA-Sender Description "SRT transport proxy sender with link aggregation"
nssm.exe set SRTLA-Sender Start SERVICE_AUTO_START
nssm.exe start SRTLA-Sender
```

## Original README Content

The content below is from the original SRTLA project:

---

The server component - srtla_rec - in this repository isunsupported, no longer under development and not suitable for production deployment. Sign up for a [BELABOX cloud](https://belabox.net/cloud) account to benefit from the latest improvements, available on a global network of relay servers.
=====

srtla - SRT transport proxy with link aggregation for connection bonding
=====

*This is srtla2, incompatible with previous versions of srtla. Remember to update srtla both on the receiver and the sender*. srtla2 brings srtla_rec support for multiple simultaneous SRT streams, and many reliability improvements both for `srtla_send` and `srtla_rec`.

This tool can transport [SRT](https://github.com/Haivision/srt/) traffic over multiple network links for capacity aggregation and redundancy. Traffic is balanced dynamically, depending on the network conditions. The intended application is bonding mobile modems for live streaming.

This application is experimental. Be prepared to troubleshoot it and experiment with various settings for your needs.

[Read the rest of the original documentation here](https://github.com/BELABOX/srtla)

## Reconnection Options

Both `srtla_rec` and `srtla_send` now include basic automatic reconnection and logging options to improve resilience when receivers or network links briefly disappear.

Command-line flags:
- `--auto-reconnect` (default enabled): enable automatic reconnection behavior.
- `--no-auto-reconnect`: disable automatic reconnection and preserve original behavior (immediate group removal on SRT failure).
- `--log-errors`: increase error verbosity to include socket error strings.
- `--reconnect-interval-ms <ms>`: base reconnect interval in milliseconds (default 500).

Behavior summary:
- `srtla_rec` will no longer immediately destroy a group on transient SRT socket failures; it will enter a WAITING_SRT state and periodically retry the SRT handshake.
- `srtla_send` will retry registrations (REG1/REG2) with exponential backoff for each source connection.
- On Windows, UDP sockets are hardened against ICMP "port unreachable" resets using `SIO_UDP_CONNRESET`.

These options are safe for backward compatibility; disabling `--auto-reconnect` restores the original behavior.

### Examples (full commands)

Copy-pasteable examples showing the positional arguments followed by flags.

- Receiver (default auto-reconnect, verbose errors):

```batch
srtla_rec.exe 5000 127.0.0.1 6262 --log-errors
```

- Receiver (disable auto-reconnect):

```batch
srtla_rec.exe 5000 127.0.0.1 6262 --no-auto-reconnect
```

- Receiver (custom reconnect interval 1000 ms):

```batch
srtla_rec.exe 5000 127.0.0.1 6262 --reconnect-interval-ms 1000 --log-errors
```

- Sender (default auto-reconnect, using a source IP list file):

```batch
srtla_send.exe 5001 192.168.1.50 5000 C:\path\to\sources.txt --log-errors
```

- Sender (disable auto-reconnect on sender):

```batch
srtla_send.exe 5001 192.168.1.50 5000 C:\path\to\sources.txt --no-auto-reconnect
```

- Sender (custom reconnect interval and verbose errors):

```batch
srtla_send.exe 5001 192.168.1.50 5000 C:\path\to\sources.txt --reconnect-interval-ms 750 --log-errors
```

Notes:
- Always put flags after the required positional arguments.
- On Linux use `./srtla_rec` / `./srtla_send` and Unix-style paths for the sources file.
