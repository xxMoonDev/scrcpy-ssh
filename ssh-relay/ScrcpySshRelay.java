import android.net.LocalSocket;
import android.net.LocalSocketAddress;

import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * scrcpy-ssh relay: bridges stdin/stdout (an SSH exec channel) with the
 * scrcpy server's abstract unix socket, replacing "adb forward".
 *
 * Usage: CLASSPATH=/data/local/tmp/scrcpy-ssh-relay.jar app_process / ScrcpySshRelay scrcpy_XXXXXXXX
 *
 * Writes a single 'K' byte once connected (consumed by the client), then
 * relays raw bytes both ways. Exits with status 1 if the socket is not
 * listening yet (the client retries), 0 when either side closes.
 */
public final class ScrcpySshRelay {

    private ScrcpySshRelay() {
    }

    private static void copy(InputStream in, OutputStream out) {
        byte[] buf = new byte[65536];
        try {
            int n;
            while ((n = in.read(buf)) != -1) {
                out.write(buf, 0, n);
                out.flush();
            }
        } catch (IOException e) {
            // connection closed
        }
    }

    public static void main(String... args) {
        if (args.length != 1) {
            System.err.println("usage: ScrcpySshRelay <abstract socket name>");
            System.exit(2);
        }

        final LocalSocket socket = new LocalSocket();
        try {
            socket.connect(new LocalSocketAddress(args[0], LocalSocketAddress.Namespace.ABSTRACT));
        } catch (IOException e) {
            System.exit(1);
        }

        // unbuffered stdout/stdin (System.out is a buffered PrintStream)
        final OutputStream stdout = new FileOutputStream(FileDescriptor.out);
        final InputStream stdin = new FileInputStream(FileDescriptor.in);
        try {
            stdout.write('K');
            stdout.flush();
            final InputStream sin = socket.getInputStream();
            final OutputStream sout = socket.getOutputStream();

            Thread up = new Thread(new Runnable() {
                @Override
                public void run() {
                    copy(stdin, sout);
                    // client closed its side: tear everything down
                    System.exit(0);
                }
            }, "relay-up");
            up.setDaemon(true);
            up.start();

            copy(sin, stdout);
        } catch (IOException e) {
            // ignore
        }
        System.exit(0);
    }
}
