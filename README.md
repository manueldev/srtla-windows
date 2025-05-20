The server component - srtla_rec - in this repository isunsupported, no longer under development and not suitable for production deployment. Sign up for a [BELABOX cloud](https://belabox.net/cloud) account to benefit from the latest improvements, available on a global network of relay servers.
=====

srtla - SRT transport proxy with link aggregation for connection bonding
=====

*This is srtla2, incompatible with previous versions of srtla. Remember to update srtla both on the receiver and the sender*. srtla2 brings srtla_rec support for multiple simultaneous SRT streams, and many reliability improvements both for `srtla_send` and `srtla_rec`.

This tool can transport [SRT](https://github.com/Haivision/srt/) traffic over multiple network links for capacity aggregation and redundancy. Traffic is balanced dynamically, depending on the network conditions. The intended application is bonding mobile modems for live streaming.

This application is experimental. Be prepared to troubleshoot it and experiment with various settings for your needs.


Assumptions and prerequisites
-----------------------------

This tool assumes that data is streamed from a SRT *sender* in *caller* mode to a SRT *receiver* in *listener* mode. To get any benefit over using SRT directly, the *sender* should have 2 or more network links to the SRT listener (in the typical application, these would be internet-connected 4G modems). The sender needs to have [source routing](https://tldp.org/HOWTO/Adv-Routing-HOWTO/lartc.rpdb.simple.html) configured, as srtla uses `bind()` to map UDP sockets to a given connection. Only Linux is supported, however porting to other *nix platforms should be straightforward if needed. Update: `srtla_rec` has now been confirmed to work on Windows via WSL1.


Building (both on the sender and the receiver)
----------------------------------------------

    git clone https://github.com/BELABOX/srtla.git
    cd srtla
    make
    
This will produce 2 executables: `srtla_send` and `srtla_rec`.


Building the patched SRT (only needed on the receiver)
------------------------------------------------------

Our patched SRT library should be used on the `receiver`. srtla will work using the upstream SRT library, however performance may be degraded because the current upstream version causes excessive retransmissions when packets arrive out of order.

    git clone https://github.com/BELABOX/srt.git
    cd srt
    ./configure
    make

See the SRT documentation for more information on building it.


Example usage
-------------

Let's assume that the receiver has IP address 10.0.0.1 and the sender has 2 (unreliable) modems with IP addresses 192.168.0.2 and 192.168.1.2 respectively, which can reach the receiver. We'll set up the srtla receiver to run on port 5000 and we'll use `srt-live-transmit` (from our SRT repository) in listener mode to make the stream available on `0.0.0.0:5001` for local or remote clients that use the upstream SRT library.

**Receiver**

    path/to/srt/srt-live-transmit -st:yes "srt://127.0.0.1:5002?mode=listener&lossmaxttl=40&latency=2000" "srt://0.0.0.0:5001?mode=listener" &
    path/to/srtla/srtla_rec 5000 127.0.0.1 5002

    
Notes: `lossmaxttl` is a required option to allow packets to arrive out-of-order without immediately sending NAKs to ask for retransmission. Its value is the size of the receive window. Values between 10 and 50 are probably a reasonable starting point. The NAKs sent by SRT are used by srtla to balance the traffic between the links and lower `lossmaxttl` values will create a stronger bias towards using the faster networks disproportionately. If the window is too small, that may cause excessive retransmissions and it may prevent link aggregation from working by sending most of the traffic through a single link. If the window is too large, it may prevent timely retransmission of lost / late / corrupted packets and therefore data loss. `latency` (in ms) will determine the time available for retransmission and packet reordering (together with `lossmaxttl`).

**Sender**

    echo 192.168.0.2 > /tmp/srtla_ips
    echo 192.168.1.2 >> /tmp/srtla_ips
    path/to/srtla/srtla_send 6000 10.0.0.1 5000 /tmp/srtla_ips
    
With `srtla_send` running on the sender, SRT-enabled applications should stream to port `6000` on the sender and this data will be forwarded through srtla and srt-live-transmit to port `5001` on the receiver.

Note that instead of `srt-live-transmit`, you can directly use the end SRT application in listener mode on the receiver. It **must** be configured with the same options discussed above for srt-live-transmit and it **should** be linked against our modified SRT library.

Note that this basic setup doesn't implement authentication or encryption and `srt-live-transmit` can only handle one connection at a time.

Note that the sender **should** implement congestion control using adaptive bitrate based on the SRT `SRTO_SNDDATA` size or on the measured `RTT`. Also note that due to reordering, these values may be slightly higher during uncongested operation over srtla compared to direct SRT operation over one of the same network links.


How does it work?
-----------------

The core idea is that srtla keeps track of the number of packets in flight (sent but unacknowledged) for each link, together with a dynamic window size that tracks the capacity of each link - similarly to TCP congestion control. These are used together to balance the traffic through each link proportionally to its capacity. However, note that no congestion control is applied.

This assumes that you're familiar with the [SRT spec](https://tools.ietf.org/html/draft-sharabayko-mops-srt-00). TODO: technical description in more detail.

**srtla v2**

The main improvement in srtla v2 is that it supports multiple *srtla senders* connecting to a single *srtla receiver* by establishing *connection groups* - note that these are different from the experimental *socket groups* in SRT. To support this feature, I've introduced a 2-phase connection registration phase.

Normal registration:

* Sender (conn 0):   `SRTLA_REG1(sender_id = SRTLA_ID_LEN bytes sender-generated random id)`
* Receiver:          `SRTLA_REG2(full_id = sender_id with the last SRTLA_ID_LEN/2 bytes replaced with receiver-generated values)`
* Sender (conn 0):   `SRTLA_REG2(full_id)`
* Receiver:          `SRTLA_REG3`
* [...]
* Sender (conn n):   `SRTLA_REG2(full_id)`
* Receiver:          `SRTLA_REG3`


Error responses are only sent from the *receiver*. If the *sender* encounters an error, it should just abandon the relevant *connection group* or *connection*, and it will be garbage collected on the receiver side after some time. Possible error responses are sent after receiving a `SRTLA_REG1` or `SRTLA_REG2` request.

* `SRTLA_REG_ERR` - Can be sent in response to any `SRTLA_REG` command. Operation temporarily failed, back off and retry later.
* `SRTLA_REG_NAK` - Sent in response to `SRTLA_REG1`. Operation refused, give up. Either incompatible or access denied. A human-readable error message may be appended after the header.
* `SRTLA_REG_NGP` - Sent in response to `SRTLA_REG2` with an invalid ID. Register the group again with `SRTLA_REG1`

## Running srtla_rec as a Windows Service

If you want to run `srtla_rec.exe` as a service on Windows, follow these detailed steps:

1. **Prepare the Executable**
   - Make sure you have a compiled `srtla_rec.exe` file. Note its full path, for example: `C:\srtla\srtla_rec.exe`.

2. **Download NSSM (Non-Sucking Service Manager)**
   - NSSM is a tool that allows you to run any application as a Windows service.
   - Download it from [https://nssm.cc/download](https://nssm.cc/download) and extract the ZIP file to a folder (e.g., `C:\nssm`).

3. **Open Command Prompt as Administrator**
   - Click Start, type `cmd`, right-click on Command Prompt, and select "Run as administrator".

4. **Install the Service with NSSM**
   - Change directory to where `nssm.exe` is located (e.g., `cd C:\nssm\win64` or `cd C:\nssm\win32` depending on your system).
   - Run the following command:
     ```cmd
     nssm install srtla_rec
     ```
   - In the NSSM GUI window that appears:
     - For **Application path**, browse and select your `srtla_rec.exe` file.
     - For **Arguments**, enter the required arguments for your use case (e.g., `5000 127.0.0.1 5002`).
     - (Optional) Set the **Startup directory** to the folder containing `srtla_rec.exe`.
     - (Optional) Under the "Details" tab, you can set a display name and description for the service.
     - (Optional) Under the "Log on" tab, you can specify which user account the service should run as (default is Local System).
     - Click "Install service" to finish.

5. **Start the Service**
   - In the same command prompt, start the service with:
     ```cmd
     net start srtla_rec
     ```
   - Alternatively, open the Services management console (`services.msc`), find `srtla_rec`, right-click and select "Start".

6. **Stop or Remove the Service**
   - To stop the service:
     ```cmd
     net stop srtla_rec
     ```
   - To remove the service completely:
     ```cmd
     nssm remove srtla_rec confirm
     ```

**Notes:**
- NSSM is only needed for the initial setup. Once installed, the service will start automatically with Windows (unless you set it to manual start).
- You can configure recovery options, environment variables, and logging in the NSSM GUI during setup.
- If you update `srtla_rec.exe`, stop the service, replace the executable, and start the service again.
- For troubleshooting, check the Windows Event Viewer or the log files if you configured logging in NSSM.

## Windows Build Support

This repository is designed to allow building and running the `srtla_rec` and `srtla_send` applications as native Windows executables. You can compile the source code on Windows using a compatible C compiler (such as MinGW or Microsoft Visual Studio). After compilation, you will have `.exe` files (`srtla_rec.exe` and `srtla_send.exe`) that can be executed directly on Windows systems, without the need for WSL or a Linux environment.

This enables seamless integration with Windows environments, including the ability to run `srtla_rec.exe` as a Windows service (see the section above for detailed instructions).
