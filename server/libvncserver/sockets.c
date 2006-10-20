/*
 * sockets.c - deal with TCP sockets.
 *
 * This code should be independent of any changes in the RFB protocol.  It just
 * deals with the X server scheduling stuff, calling rfbNewClientConnection and
 * rfbProcessClientMessage to actually deal with the protocol.  If a socket
 * needs to be closed for any reason then rfbCloseClient should be called, and
 * this in turn will call rfbClientConnectionGone.  To make an active
 * connection out, call rfbConnect - note that this does _not_ call
 * rfbNewClientConnection.
 *
 * This file is divided into two types of function.  Those beginning with
 * "rfb" are specific to sockets using the RFB protocol.  Those without the
 * "rfb" prefix are more general socket routines (which are used by the http
 * code).
 *
 * Thanks to Karl Hakimian for pointing out that some platforms return EAGAIN
 * not EWOULDBLOCK.
 */

/*
 *  OSXvnc Copyright (C) 2001 Dan McGuirk <mcguirk@incompleteness.net>.
 *  Original Xvnc code Copyright (C) 1999 AT&T Laboratories Cambridge.  
 *  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

#include <rfb/rfb.h>

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef WIN32
#pragma warning (disable: 4018 4761)
#define close closesocket
#define read(sock,buf,len) recv(sock,buf,len,0)
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ETIMEDOUT WSAETIMEDOUT
#define write(sock,buf,len) send(sock,buf,len,0)
#else
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#endif

#if defined(__linux__) && defined(NEED_TIMEVAL)
struct timeval 
{
   long int tv_sec,tv_usec;
}
;
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <errno.h>

/*#ifndef WIN32
int max(int i,int j) { return(i<j?j:i); }
#endif
*/

int rfbMaxClientWait = 20000;   /* time (ms) after which we decide client has
                                   gone away - needed to stop us hanging */

/*
 * rfbInitSockets sets up the TCP sockets to listen for RFB
 * connections.  It does nothing if called again.
 */

static void rfbInitListenSock(rfbScreenInfoPtr rfbScreen);

void
rfbInitSockets(rfbScreenInfoPtr rfbScreen)
{
    if (rfbScreen->socketInitDone)
	return;

    rfbScreen->socketInitDone = TRUE;

    if (rfbScreen->inetdSock != -1) {
	const int one = 1;

#ifndef WIN32
	if (fcntl(rfbScreen->inetdSock, F_SETFL, O_NONBLOCK) < 0) {
	    rfbLogPerror("fcntl");
	    return;
	}
#endif

	if (setsockopt(rfbScreen->inetdSock, IPPROTO_TCP, TCP_NODELAY,
		       (char *)&one, sizeof(one)) < 0) {
	    rfbLogPerror("setsockopt");
	    return;
	}

    	FD_ZERO(&(rfbScreen->allFds));
    	FD_SET(rfbScreen->inetdSock, &(rfbScreen->allFds));
    	rfbScreen->maxFd = rfbScreen->inetdSock;
	return;
    }

    rfbInitListenSock(rfbScreen);
}

static void
rfbInitListenSock(rfbScreenInfoPtr rfbScreen)
{
    if(rfbScreen->autoPort) {
        int i;
        rfbLog("Autoprobing TCP port \n");

        for (i = 5900; i < 6000; i++) {
            if ((rfbScreen->rfbListenSock = ListenOnTCPPort(i, rfbScreen->localOnly)) >= 0) {
		rfbScreen->rfbPort = i;
		break;
	    }
        }

        if (i >= 6000) {
	    rfbLogPerror("Failure autoprobing");
	    return;
        }

        rfbLog("Autoprobing selected port %d\n", rfbScreen->rfbPort);
        FD_ZERO(&(rfbScreen->allFds));
        FD_SET(rfbScreen->rfbListenSock, &(rfbScreen->allFds));
        rfbScreen->maxFd = rfbScreen->rfbListenSock;
    }
    else if(rfbScreen->rfbPort>0) {
      rfbLog("Listening for VNC connections on TCP port %d\n", rfbScreen->rfbPort);

      if ((rfbScreen->rfbListenSock = ListenOnTCPPort(rfbScreen->rfbPort, rfbScreen->localOnly)) < 0) {
	rfbLogPerror("ListenOnTCPPort");
	return;
      }

      FD_ZERO(&(rfbScreen->allFds));
      FD_SET(rfbScreen->rfbListenSock, &(rfbScreen->allFds));
      rfbScreen->maxFd = rfbScreen->rfbListenSock;
    }
}

void
rfbSetAutoPort(rfbScreenInfoPtr rfbScreen, rfbBool autoPort)
{
    if (rfbScreen->autoPort == autoPort)
        return;

    rfbScreen->autoPort = autoPort;

    if (!rfbScreen->socketInitDone)
	return;

    if (rfbScreen->rfbListenSock > 0) {
        FD_CLR(rfbScreen->rfbListenSock, &(rfbScreen->allFds));
        close(rfbScreen->rfbListenSock);
        rfbScreen->rfbListenSock = -1;
    }

    rfbInitListenSock(rfbScreen);
}

void
rfbSetPort(rfbScreenInfoPtr rfbScreen, int port)
{
    if (rfbScreen->rfbPort == port)
        return;

    rfbScreen->rfbPort = port;

    if (!rfbScreen->socketInitDone || rfbScreen->autoPort)
	return;

    if (rfbScreen->rfbListenSock > 0) {
        FD_CLR(rfbScreen->rfbListenSock, &(rfbScreen->allFds));
        close(rfbScreen->rfbListenSock);
        rfbScreen->rfbListenSock = -1;
    }

    rfbInitListenSock(rfbScreen);
}

void
rfbSetLocalOnly(rfbScreenInfoPtr rfbScreen, rfbBool localOnly)
{
    if (rfbScreen->localOnly == localOnly)
        return;

    rfbScreen->localOnly = localOnly;

    if (!rfbScreen->socketInitDone)
	return;

    if (rfbScreen->rfbListenSock > 0) {
        FD_CLR(rfbScreen->rfbListenSock, &(rfbScreen->allFds));
        close(rfbScreen->rfbListenSock);
        rfbScreen->rfbListenSock = -1;
    }

    rfbLog("Re-binding socket to listen for %s VNC connections on TCP port %d\n",
           rfbScreen->localOnly ? "local" : "all", rfbScreen->rfbPort);

    if ((rfbScreen->rfbListenSock = ListenOnTCPPort(rfbScreen->rfbPort, rfbScreen->localOnly)) < 0) {
	rfbLogPerror("ListenOnTCPPort");
	return;
    }

    FD_SET(rfbScreen->rfbListenSock, &(rfbScreen->allFds));
    rfbScreen->maxFd = max(rfbScreen->rfbListenSock, rfbScreen->maxFd);
}

void
rfbProcessNewConnection(rfbScreenInfoPtr rfbScreen)
{
    const int one = 1;
    int sock;

    if ((sock = accept(rfbScreen->rfbListenSock, NULL, NULL)) < 0) {
	rfbLogPerror("rfbCheckFds: accept");
	return;
    }

#ifndef WIN32
    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
	rfbLogPerror("rfbCheckFds: fcntl");
	close(sock);
	return;
    }
#endif

    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
		   (char *)&one, sizeof(one)) < 0) {
	rfbLogPerror("rfbCheckFds: setsockopt");
	close(sock);
	return;
    }

    rfbNewClient(rfbScreen,sock);
}

/*
 * rfbCheckFds is called from ProcessInputEvents to check for input on the RFB
 * socket(s).  If there is input to process, the appropriate function in the
 * RFB server code will be called (rfbNewClientConnection,
 * rfbProcessClientMessage, etc).
 */

void
rfbCheckFds(rfbScreenInfoPtr rfbScreen,long usec)
{
    int nfds;
    fd_set fds;
    struct timeval tv;
    rfbClientIteratorPtr i;
    rfbClientPtr cl;

    if (!rfbScreen->inetdInitDone && rfbScreen->inetdSock != -1) {
	rfbNewClientConnection(rfbScreen,rfbScreen->inetdSock); 
	rfbScreen->inetdInitDone = TRUE;
    }

    memcpy((char *)&fds, (char *)&(rfbScreen->allFds), sizeof(fd_set));
    tv.tv_sec = 0;
    tv.tv_usec = usec;
    nfds = select(rfbScreen->maxFd + 1, &fds, NULL, NULL /* &fds */, &tv);
    if (nfds == 0) {
	return;
    }
    if (nfds < 0) {
#ifdef WIN32
		errno = WSAGetLastError();
#endif
	if (errno != EINTR)
		rfbLogPerror("rfbCheckFds: select");
	return;
    }

    if (rfbScreen->rfbListenSock != -1 && FD_ISSET(rfbScreen->rfbListenSock, &fds)) {
	rfbProcessNewConnection(rfbScreen);
	FD_CLR(rfbScreen->rfbListenSock, &fds);
	if (--nfds == 0)
	    return;
    }

    i = rfbGetClientIterator(rfbScreen);
    while((cl = rfbClientIteratorNext(i))) {
      if (cl->onHold)
	continue;
      if (FD_ISSET(cl->sock, &fds) && FD_ISSET(cl->sock, &(rfbScreen->allFds)))
	rfbProcessClientMessage(cl);
    }
    rfbReleaseClientIterator(i);
}

void
rfbCloseClient(cl)
     rfbClientPtr cl;
{
    rfbAuthCleanupClient(cl);

    LOCK(cl->updateMutex);
#ifdef LIBVNCSERVER_HAVE_LIBPTHREAD
    if (cl->sock != -1)
#endif
      {
	FD_CLR(cl->sock,&(cl->screen->allFds));
	if(cl->sock==cl->screen->maxFd)
	  while(cl->screen->maxFd>0
		&& !FD_ISSET(cl->screen->maxFd,&(cl->screen->allFds)))
	    cl->screen->maxFd--;
	shutdown(cl->sock,SHUT_RDWR);
	close(cl->sock);
	cl->sock = -1;
      }
    UNLOCK(cl->updateMutex);
}


#ifdef HAVE_GNUTLS
static int
ReadExactOverTLS(rfbClientPtr cl, char* buf, int len, int timeout)
{
    while (len > 0) {
	int n;

	n = gnutls_record_recv(cl->tlsSession, buf, len);
	if (n == 0) {
	    UNLOCK(cl->outputMutex);
	    return 0;

	} else if (n < 0) {
	    if (n == GNUTLS_E_INTERRUPTED || n == GNUTLS_E_AGAIN)
		continue;

	    UNLOCK(cl->outputMutex);
	    return -1;
	}

	buf += n;
	len -= n;
    }

    return 1;
}
#endif /* HAVE_GNUTLS */

/*
 * ReadExact reads an exact number of bytes from a client.  Returns 1 if
 * those bytes have been read, 0 if the other end has closed, or -1 if an error
 * occurred (errno is set to ETIMEDOUT if it timed out).
 */

int
ReadExactTimeout(rfbClientPtr cl, char* buf, int len, int timeout)
{
    int sock = cl->sock;
    int n;
    fd_set fds;
    struct timeval tv;

#ifdef HAVE_GNUTLS
    if (cl->useTLS)
	return ReadExactOverTLS(cl, buf, len, timeout);
#endif

    while (len > 0) {
        n = read(sock, buf, len);

        if (n > 0) {

            buf += n;
            len -= n;

        } else if (n == 0) {

            return 0;

        } else {
#ifdef WIN32
			errno = WSAGetLastError();
#endif
	    if (errno == EINTR)
		continue;

            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                return n;
            }

            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            tv.tv_sec = timeout / 1000;
            tv.tv_usec = (timeout % 1000) * 1000;
            n = select(sock+1, &fds, NULL, &fds, &tv);
            if (n < 0) {
                rfbLogPerror("ReadExact: select");
                return n;
            }
            if (n == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
    }
    return 1;
}

int ReadExact(rfbClientPtr cl,char* buf,int len)
{
  return(ReadExactTimeout(cl,buf,len,rfbMaxClientWait));
}

#ifdef HAVE_GNUTLS
static int
WriteExactOverTLS(rfbClientPtr cl, const char* buf, int len)
{
    LOCK(cl->outputMutex);

    while (len > 0) {
	int n;

	n = gnutls_record_send(cl->tlsSession, buf, len);
	if (n == 0) {
	    UNLOCK(cl->outputMutex);
	    return 0;

	} else if (n < 0) {
	    if (n == GNUTLS_E_INTERRUPTED || n == GNUTLS_E_AGAIN)
		continue;

	    UNLOCK(cl->outputMutex);
	    return -1;
	}

	buf += n;
	len -= n;
    }

    UNLOCK(cl->outputMutex);

    return 1;
}
#endif /* HAVE_GNUTLS */

/*
 * WriteExact writes an exact number of bytes to a client.  Returns 1 if
 * those bytes have been written, or -1 if an error occurred (errno is set to
 * ETIMEDOUT if it timed out).
 */

int
WriteExact(rfbClientPtr cl, const char* buf, int len)
{
    int sock = cl->sock;
    int n;
    fd_set fds;
    struct timeval tv;
    int totalTimeWaited = 0;

#ifdef HAVE_GNUTLS
    if (cl->useTLS)
	return WriteExactOverTLS(cl, buf, len);
#endif

    LOCK(cl->outputMutex);
    while (len > 0) {
        n = write(sock, buf, len);

        if (n > 0) {

            buf += n;
            len -= n;

        } else if (n == 0) {

            rfbErr("WriteExact: write returned 0?\n");
            return 0;

        } else {
#ifdef WIN32
			errno = WSAGetLastError();
#endif
	    if (errno == EINTR)
		continue;

            if (errno != EWOULDBLOCK && errno != EAGAIN) {
	        UNLOCK(cl->outputMutex);
                return n;
            }

            /* Retry every 5 seconds until we exceed rfbMaxClientWait.  We
               need to do this because select doesn't necessarily return
               immediately when the other end has gone away */

            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            n = select(sock+1, NULL, &fds, NULL /* &fds */, &tv);
            if (n < 0) {
                rfbLogPerror("WriteExact: select");
                UNLOCK(cl->outputMutex);
                return n;
            }
            if (n == 0) {
                totalTimeWaited += 5000;
                if (totalTimeWaited >= rfbMaxClientWait) {
                    errno = ETIMEDOUT;
                    UNLOCK(cl->outputMutex);
                    return -1;
                }
            } else {
                totalTimeWaited = 0;
            }
        }
    }
    UNLOCK(cl->outputMutex);
    return 1;
}

int
ListenOnTCPPort(port, localOnly)
    int port;
    rfbBool localOnly;
{
    int sock = -1;
    int one = 1;
    struct sockaddr_in addr_in;
    struct sockaddr *addr;
    socklen_t addrlen;

#ifdef ENABLE_IPV6
    struct sockaddr_in6 addr_in6;

    memset(&addr_in6, 0, sizeof(addr_in6));
    addr_in6.sin6_family = AF_INET6;
    addr_in6.sin6_port = htons(port);
    addr_in6.sin6_addr = localOnly ? in6addr_loopback : in6addr_any;

    addr = (struct sockaddr *)&addr_in6;
    addrlen = sizeof(addr_in6);

    sock = socket(AF_INET6, SOCK_STREAM, 0);
#endif

    if (sock < 0) {
        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
            return -1;

        memset(&addr_in, 0, sizeof(addr_in));
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(port);
        addr_in.sin_addr.s_addr = localOnly ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

        addr = (struct sockaddr *)&addr_in;
        addrlen = sizeof(addr_in);
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
		   (char *)&one, sizeof(one)) < 0) {
	close(sock);
	return -1;
    }
    if (bind(sock, addr, addrlen) < 0) {
	close(sock);
	return -1;
    }
    if (listen(sock, 5) < 0) {
	close(sock);
	return -1;
    }

    return sock;
}
