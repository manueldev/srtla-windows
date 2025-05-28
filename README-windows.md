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
