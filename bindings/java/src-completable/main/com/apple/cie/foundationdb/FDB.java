/*
 * FDB.java
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.apple.cie.foundationdb;

import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadFactory;

/**
 * The starting point for accessing FoundationDB.
 *  <br>
 *  <h3>Setting API version</h3>
 *  The FoundationDB API is accessed with a call to {@link #selectAPIVersion(int)}.
 *   This call is required before using any other part of the API. The call allows
 *   an error to be thrown at this point to prevent client code from accessing a later library
 *   with incorrect assumptions from the current version. The API version documented here is version
 *   {@code 500}.<br><br>
 *  FoundationDB encapsulates multiple versions of its interface by requiring
 *   the client to explicitly specify the version of the API it uses. The purpose
 *   of this design is to allow you to upgrade the server, client libraries, or
 *   bindings without having to modify client code. The client libraries support
 *   all previous versions of the API. The API version specified by the client is
 *   used to control the behavior of the binding. You can therefore upgrade to
 *   more recent packages (and thus receive various improvements) without having
 *   to change your code.<br><br>
 *  Warning: When using the multi-version client API, setting an API version that
 *   is not supported by a particular client library will prevent that client from 
 *   being used to connect to the cluster. In particular, you should not advance
 *   the API version of your application after upgrading your client until the 
 *   cluster has also been upgraded.<br>
 *  <h3>Getting a database</h3>
 *  Once the API version has been set, the easiest way to get a {@link Database} object to use is
 *   to call {@link #open}.
 *  <br>
 *  <h3>Client networking</h3>
 *  The network is started either implicitly with a call to a variant of {@link #open()} or
 *   {@link #createCluster()}, or started explicitly with a call to {@link #startNetwork()}.
 *  <br>
 *
 */
public class FDB {
	static FDB singleton = null;

	static class DaemonThreadFactory implements ThreadFactory {
		private final ThreadFactory factory;

		public DaemonThreadFactory(ThreadFactory factory) {
			this.factory = factory;
		}

		@Override
		public Thread newThread(Runnable r) {
			Thread t = factory.newThread(r);
			t.setDaemon(true);
			return t;
		}
	}

	public static final ExecutorService DEFAULT_EXECUTOR;

	final int apiVersion;
	private volatile boolean netStarted = false;
	private volatile boolean netStopped = false;
	final private Semaphore netRunning = new Semaphore(1);
	private final NetworkOptions options;

	static {
		try {
			JNIUtil.loadLibrary("fdb_c");
		} catch (Throwable t) {
			// EAT: this can be useful for loading on windows
		}
		JNIUtil.loadLibrary("fdb_java");

		ThreadFactory factory = new DaemonThreadFactory(Executors.defaultThreadFactory());
		DEFAULT_EXECUTOR = Executors.newCachedThreadPool(factory);
	}

	/**
	 * Called only once to create the FDB singleton.
	 */
	private FDB(int apiVersion) {
		this.apiVersion = apiVersion;

		options = new NetworkOptions(new OptionConsumer() {
			@Override
			public void setOption(int code, byte[] parameter) {
				Network_setOption(code, parameter);
			}
		});

		Runtime.getRuntime().addShutdownHook(new Thread(
			new Runnable(){
				@Override
				public void run() {
					FDB.this.stopNetwork();
				}
			}
		));
	}

	/**
	 * Returns a set of options that can be set on a the FoundationDB API. Generally,
	 *  these options to the top level of the API affect the networking engine and
	 *  therefore must be set before the network engine is started. The network is started
	 *  by calls to {@link #startNetwork()} and implicitly by calls to {@link #open()} and
	 *  {@link #createCluster()} (and their respective variants).
	 *
	 * @return a set of options affecting this instance of the FoundationDB API
	 */
	public NetworkOptions options() { return options; }

	/**
	 * Select the version for the client API. An exception will be thrown if the
	 *  requested version is not supported by this implementation of the API. As
	 *  only one version can be selected for the lifetime of the JVM, the result
	 *  of a successful call to this method is always the same instance of a FDB
	 *  object.<br><br>
	 *
	 *  Warning: When using the multi-version client API, setting an API version that
	 *   is not supported by a particular client library will prevent that client from 
	 *   being used to connect to the cluster. In particular, you should not advance
	 *   the API version of your application after upgrading your client until the 
	 *   cluster has also been upgraded.
	 *
	 * @param version the API version required
	 *
	 * @return the FoundationDB API object
	 */
	public synchronized static FDB selectAPIVersion(final int version) throws FDBException {
		if(singleton != null) {
			if(version != singleton.apiVersion) {
				throw new IllegalArgumentException(
						"FoundationDB API already started at different version");
			}
			return singleton;
		}
		if(version < 500)
			throw new IllegalArgumentException("API version not supported (minimum 500)");
		if(version > 500)
			throw new IllegalArgumentException("API version not supported (maximum 500)");
		Select_API_version(version);
		return singleton = new FDB(version);
	}

	/**
	 * Connects to the cluster specified by the
	 *  <a href="https://foundationdb.com/documentation/api-general.html#default-cluster-file" target="_blank">default fdb.cluster file</a>.
	 *  If the FoundationDB network has not been started, it will be started in the course of this call
	 *  as if {@link FDB#startNetwork()} had been called.
	 *
	 * @return a {@code CompletableFuture} that will be set to a FoundationDB {@code Cluster}.
	 *
	 * @throws FDBException on errors encountered starting the FoundationDB networking engine
	 * @throws IllegalStateException if the network had been previously stopped
	 */
	public Cluster createCluster() throws IllegalStateException, FDBException {
		return createCluster(null);
	}

	/**
	 * Connects to the cluster specified by {@code clusterFilePath}. If the FoundationDB network
	 *  has not been started, it will be started in the course of this call as if
	 *  {@link #startNetwork()} had been called.
	 *
	 * @param clusterFilePath the
	 *  <a href="/documentation/api-general.html#foundationdb-cluster-file" target="_blank">cluster file</a>
	 *  defining the FoundationDB cluster. This can be {@code null} if the
	 *  <a href="/documentation/api-general.html#default-cluster-file" target="_blank">default fdb.cluster file</a>
	 *  is to be used.
	 *
	 * @return a {@code CompletableFuture} that will be set to a FoundationDB {@code Cluster}.
	 *
	 * @throws FDBException on errors encountered starting the FoundationDB networking engine
	 * @throws IllegalStateException if the network had been previously stopped
	 */
	public Cluster createCluster(String clusterFilePath) throws IllegalStateException, FDBException {
		return createCluster(clusterFilePath, DEFAULT_EXECUTOR);
	}

	/**
	 * Connects to the cluster specified by {@code clusterFilePath}. If the FoundationDB network
	 *  has not been started, it will be started in the course of this call. The supplied
	 *  {@link Executor} will be used as the default for the execution of all callbacks that
	 *  are produced from using the resulting {@link Cluster}.
	 *
	 * @param clusterFilePath the
	 *  <a href="/documentation/api-general.html#foundationdb-cluster-file" target="_blank">cluster file</a>
	 *  defining the FoundationDB cluster. This can be {@code null} if the
	 *  <a href="/documentation/api-general.html#default-cluster-file" target="_blank">default fdb.cluster file</a>
	 *  is to be used.
	 * @param e used to run the FDB network thread
	 *
	 * @return a {@code CompletableFuture} that will be set to a FoundationDB {@code Cluster}.
	 *
	 * @throws FDBException on errors encountered starting the FoundationDB networking engine
	 * @throws IllegalStateException if the network had been previously stopped
	 */
	public Cluster createCluster(String clusterFilePath, Executor e)
			throws FDBException, IllegalStateException {
		FutureCluster f;
		synchronized (this) {
			if (!isConnected()) {
				startNetwork(e);
			}
			f = new FutureCluster(Cluster_create(clusterFilePath), e);
		}
		return f.join();
	}

	/**
	 * Initializes networking, connects with the
	 *  <a href="https://foundationdb.com/documentation/api-general.html#default-cluster-file" target="_blank">default fdb.cluster file</a>,
	 *  and opens the database.
	 *
	 * @return a {@code CompletableFuture} that will be set to a FoundationDB {@link Database}
	 */
	public Database open() throws FDBException {
		return open(null);
	}

	/**
	 * Initializes networking, connects to the cluster specified by {@code clusterFilePath}
	 *  and opens the database.
	 *
	 * @param clusterFilePath the
	 *  <a href="/documentation/api-general.html#foundationdb-cluster-file" target="_blank">cluster file</a>
	 *  defining the FoundationDB cluster. This can be {@code null} if the
	 *  <a href="/documentation/api-general.html#default-cluster-file" target="_blank">default fdb.cluster file</a>
	 *  is to be used.
	 *
	 * @return a {@code CompletableFuture} that will be set to a FoundationDB {@link Database}
	 */
	public Database open(String clusterFilePath) throws FDBException {
		return open(clusterFilePath, DEFAULT_EXECUTOR);
	}

	/**
	 * Initializes networking, connects to the cluster specified by {@code clusterFilePath}
	 *  and opens the database.
	 *
	 * @param clusterFilePath the
	 *  <a href="/documentation/api-general.html#foundationdb-cluster-file" target="_blank">cluster file</a>
	 *  defining the FoundationDB cluster. This can be {@code null} if the
	 *  <a href="/documentation/api-general.html#default-cluster-file" target="_blank">default fdb.cluster file</a>
	 *  is to be used.
	 * @param e the {@link Executor} to use to execute asynchronous callbacks
	 *
	 * @return a {@code CompletableFuture} that will be set to a FoundationDB {@link Database}
	 */
	public Database open(String clusterFilePath, Executor e) throws FDBException {
		FutureCluster f;
		synchronized (this) {
			if (!isConnected()) {
				startNetwork();
			}
			f = new FutureCluster(Cluster_create(clusterFilePath), e);
		}
		Cluster c = f.join();
		return c.openDatabase(e);
	}

	/**
	 * Initializes networking. Can only be called once. This version of
	 * {@code startNetwork()} will use the default thread pool to execute the
	 * FoundationDB network.<br>
	 * <br>
	 * Configuration of the networking engine can be achieved through calls to the methods
	 *  in {@link NetworkOptions}.
	 *
	 * @throws IllegalStateException if the network has already been stopped
	 *
	 * @see NetworkOptions
	 */
	public void startNetwork() throws FDBException, IllegalStateException {
		startNetwork(DEFAULT_EXECUTOR);
	}

	/**
	 * Initializes networking. Can only be called once. The FoundationDB
	 * networking event loop will be run in the specified {@code Executor}. This
	 * event loop is a blocking operation that is not
	 * expected to terminate until the program in complete. This will therefore consume an
	 * entire thread from {@code e} if {@code e} is a thread pool or will completely block
	 * operation of a single threaded {@code Executor}.<br>
	 * <br>
	 * Manual configuration of the networking engine can be achieved through calls on
	 *  {@link NetworkOptions}. These options should be set before a call
	 *  to this method.
	 *
	 * @see NetworkOptions
	 *
	 * @throws IllegalStateException if the network has already been stopped
	 */
	public synchronized void startNetwork(Executor e) throws FDBException, IllegalStateException {
		if(netStopped)
			throw new IllegalStateException("Network has been stopped and cannot be restarted");
		if(netStarted) {
			return;
		}
		Network_setup();
		netStarted = true;

		e.execute(new Runnable() {
			@Override
			public void run() {
				boolean acquired = false;
				try {
					while(!acquired) {
						try {
							// make attempt to avoid a needless deadlock
							synchronized (FDB.this) {
								if(netStopped) {
									return;
								}
							}

							netRunning.acquire();
							acquired = true;
						} catch(InterruptedException e) {}
					}
					try {
						Network_run();
					} catch (Throwable t) {
						System.err.println("Unhandled error in FoundationDB network thread: " + t.getMessage());
						// eat this error. we have nowhere to send it.
					}
				} finally {
					if(acquired) {
						netRunning.release();
					}
					synchronized (FDB.this) {
						netStopped = true;
					}
				}
			}
		});
	}

	/**
	 * Gets the state of the FoundationDB networking thread.
	 *
	 * @return {@code true} if the FDB network thread is running, {@code false} otherwise.
	 */
	private synchronized boolean isConnected() {
		return netStarted && !netStopped;
	}

	/**
	 * Stops the FoundationDB networking engine. This can be called only once -- the network
	 *  cannot be restarted after this call. This call blocks for the completion of
	 *  the FoundationDB networking engine.
	 *
	 * @throws FDBException on errors while stopping the network
	 */
	public synchronized void stopNetwork() throws FDBException {
		if(!netStarted || netStopped) {
			netStopped = true;
			return;
		}
		Network_stop();
		// set netStarted here in case the network has never really ever been run
		netStopped = netStarted = true;
		while(true) {
			try {
				// This will be released when runNetwork() returns.
				// Taking this and never releasing it will also assure
				//  that we will never again be able to call runNetwork()
				netRunning.acquire();
				return;
			} catch (InterruptedException e) {}
		}
	}

	protected static boolean evalErrorPredicate(int predicate, int code) {
		if(singleton == null)
			throw new IllegalStateException("FDB API not yet initalized");
		return singleton.Error_predicate(predicate, code);
	}

	static native void Select_API_version(int version) throws FDBException;

	private native void Network_setOption(int code, byte[] value) throws FDBException;
	private native void Network_setup() throws FDBException;
	private native void Network_run() throws FDBException;
	private native void Network_stop() throws FDBException;

	private native boolean Error_predicate(int predicate, int code);

	private native long Cluster_create(String clusterFileName);
}
