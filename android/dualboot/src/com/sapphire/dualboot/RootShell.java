package com.sapphire.dualboot;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

/**
 * One su process kept open for block reads: each read pipes dd through base64 followed by a marker line,
 * so many small reads (HFS+ B-tree nodes) cost a dd each instead of a new su, and missing devices or short
 * reads fail cleanly with IOException instead of deadlocking on raw byte counts. Reads only.
 */
final class RootShell {
	private static final String MARKER = "__SAPPHIRE_END__";
	private static Process proc;
	private static InputStream in;
	private static OutputStream out;

	private RootShell() {
	}

	private static synchronized void open() throws IOException {
		if (proc != null && proc.isAlive()) {
			return;
		}
		proc = new ProcessBuilder("su").redirectErrorStream(false).start();
		in = proc.getInputStream();
		out = proc.getOutputStream();
		final InputStream err = proc.getErrorStream();
		Thread drain = new Thread(() -> {
			byte[] discard = new byte[4096];
			try {
				while (err.read(discard) >= 0) {
					/* drain su stderr so pipe buffer never blocks */
				}
			} catch (IOException ignored) {
			}
		}, "sapphire-su-stderr");
		drain.setDaemon(true);
		drain.start();
	}

	/** COUNT bytes of DEVICE from byte OFFSET (OFFSET and COUNT multiples of 512). */
	static synchronized byte[] read(String device, long offset, int count) throws IOException {
		if (offset < 0 || offset % 512 != 0 || count % 512 != 0 || count <= 0) {
			throw new IOException(L.t("lectura no alineada", "unaligned read"));
		}
		open();
		int bs = (offset % 4096 == 0 && count % 4096 == 0) ? 4096 : 512;
		String quoted = "'" + device.replace("'", "'\\''") + "'";
		String cmd = "dd if=" + quoted + " bs=" + bs + " skip=" + (offset / bs) + " count=" + (count / bs)
		    + " 2>/dev/null | base64; echo; echo " + MARKER + "\n";
		try {
			out.write(cmd.getBytes(StandardCharsets.UTF_8));
			out.flush();
		} catch (IOException e) {
			close();
			throw e;
		}
		int maxChars = (count / 3 + 2) * 4 + (count / 48) + 256;
		StringBuilder b64 = new StringBuilder(Math.min(maxChars, 65536));
		StringBuilder line = new StringBuilder(128);
		boolean done = false;
		try {
			for (int c; (c = in.read()) >= 0; ) {
				if (c == '\n') {
					int len = line.length();
					if (len > 0 && line.charAt(len - 1) == '\r') {
						line.setLength(--len);
					}
					String s = line.toString();
					line.setLength(0);
					if (s.equals(MARKER)) {
						done = true;
						break;
					}
					if (!s.isEmpty()) {
						if (b64.length() + s.length() > maxChars) {
							close();
							throw new IOException(L.t("respuesta excesiva de ", "oversized read from ") + device);
						}
						b64.append(s);
					}
				} else {
					line.append((char) c);
					if (line.length() > maxChars) {
						close();
						throw new IOException(L.t("línea excesiva de ", "oversized line from ") + device);
					}
				}
			}
		} catch (IOException e) {
			close();
			throw e;
		}
		if (!done) {
			close();
			throw new IOException(L.t("el shell root se cerró", "the root shell closed"));
		}
		byte[] buf;
		try {
			buf = b64.length() == 0 ? new byte[0] : Base64.getMimeDecoder().decode(b64.toString());
		} catch (IllegalArgumentException e) {
			close();
			throw new IOException(L.t("datos corruptos al leer ", "corrupt base64 reading ") + device, e);
		}
		if (buf.length != count) {
			throw new IOException(L.t("no se pudo leer ", "could not read ") + device + " (" + buf.length + "/" + count + " bytes)");
		}
		return buf;
	}

	static synchronized void close() {
		if (out != null) {
			try {
				out.close();
			} catch (IOException ignored) {
			}
		}
		if (in != null) {
			try {
				in.close();
			} catch (IOException ignored) {
			}
		}
		if (proc != null) {
			proc.destroy();
		}
		proc = null;
		in = null;
		out = null;
	}
}
