package org.msoar.sps.sm;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.net.Socket;
import java.util.List;
import java.util.Map;

import org.apache.log4j.Logger;
import org.msoar.sps.SharedNames;

final class RemoteRunner implements Runner {
	private static final Logger logger = Logger.getLogger(RemoteRunner.class);
	
	private final String component;
	private final ObjectOutputStream oout;
	private final ObjectInputStream oin;
	
	RemoteRunner(Socket socket) throws IOException {
		this.oout = new ObjectOutputStream(new BufferedOutputStream(socket.getOutputStream()));
		this.oout.flush();
		
		this.oin = new ObjectInputStream(new BufferedInputStream(socket.getInputStream()));
		
		logger.debug("new remote runner waiting for component name");
		this.component = Runners.readString(oin);
		if (component == null) {
			throw new IOException();
		}

		logger.debug("'" + component + "' received, writing ok");
		oout.writeObject(SharedNames.NET_OK);
		this.oout.flush();
	}

	public void configure(List<String> command, String config, Map<String, String> environment) throws IOException {
		oout.writeObject(SharedNames.NET_CONFIGURE);
		oout.writeObject(command.toArray(new String[command.size()]));
		if (config == null) {
			oout.writeObject(SharedNames.NET_CONFIG_NO);
		} else {
			oout.writeObject(SharedNames.NET_CONFIG_YES);
			oout.writeObject(config);
		}
		
		if (environment == null) {
			oout.writeObject(SharedNames.NET_ENVIRONMENT_NO);
		} else {
			oout.writeObject(SharedNames.NET_ENVIRONMENT_YES);
			oout.writeObject(environment.keySet().toArray(new String[environment.keySet().size()]));
			oout.writeObject(environment.values().toArray(new String[environment.values().size()]));
		}

		oout.flush();
	}

	public void stop() throws IOException {
		oout.writeObject(SharedNames.NET_STOP);
		oout.flush();
	}

	public void quit() {
		try {
			oout.writeObject(SharedNames.NET_QUIT);
			oout.flush();
			oout.close();
			oin.close();
		} catch (IOException ignored) {
		}
	}

	public String getComponentName() {
		return component;
	}

	public boolean isAlive() throws IOException {
		Boolean aliveResponse = null;
		oout.writeObject(SharedNames.NET_ALIVE);
		oout.flush();
		aliveResponse = Runners.readBoolean(oin);

		if (aliveResponse == null) {
			throw new IOException();
		}
		
		return aliveResponse;
	}

	public void start() throws IOException {
		oout.writeObject(SharedNames.NET_START);
		oout.flush();
	}

	private final static class OutputPump implements Runnable {
		private final BufferedReader output;
		private final String component;
		
		private OutputPump(BufferedReader output, String component) {
			this.output = output;
			this.component = component;
		}
		
		public void run() {
			logger.debug(component + ": output pump alive");
			String out;
			try {
				while (( out = output.readLine()) != null) {
					System.out.println(out);
				}
			} catch (IOException e) {
				logger.error(e.getMessage());
			}
		}
	}
	
	public void setOutput(BufferedReader output) {
		Thread thread = new Thread(new OutputPump(output, component));
		thread.setDaemon(true);
		thread.start();
	}
}
