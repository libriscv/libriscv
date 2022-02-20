/*
 * tcpserver.c - A simple TCP echo server
 * usage: tcpserver <port>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUFSIZE 1024
#define PORT_NO 8081

/*
 * error - wrapper for perror
 */
void error(const char *msg) {
	perror(msg);
	exit(1);
}

int main()
{
	int parentfd; /* parent socket */
	int childfd; /* child socket */
	socklen_t clientlen; /* byte size of client's address */
	struct sockaddr_in serveraddr; /* server's addr */
	struct sockaddr_in clientaddr; /* client addr */
	char buf[BUFSIZE]; /* message buffer */
	char *hostaddrp; /* dotted decimal host addr string */

	/*
	* socket: create the parent socket
	*/
	parentfd = socket(AF_INET, SOCK_STREAM, 0);
	if (parentfd < 0)
		error("ERROR opening socket");

	/* setsockopt: Handy debugging trick that lets
	* us rerun the server immediately after we kill it;
	* otherwise we have to wait about 20 secs.
	* Eliminates "ERROR on binding: Address already in use" error.
	*/
	const int optval = 1;
	setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR,
		&optval , sizeof(int));

	/*
	* build the server's Internet address
	*/
	memset(&serveraddr, 0, sizeof(serveraddr));

	/* this is an Internet address */
	serveraddr.sin_family = AF_INET;

	/* let the system figure out our IP address */
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

	/* this is the port we will listen on */
	serveraddr.sin_port = htons((unsigned short)PORT_NO);

	/*
	* bind: associate the parent socket with a port
	*/
	if (bind(parentfd, (struct sockaddr *) &serveraddr,
		sizeof(serveraddr)) < 0)
	error("ERROR on binding");

	/*
	* listen: make this socket ready to accept connection requests
	*/
	if (listen(parentfd, 5) < 0) /* allow 5 requests to queue up */
		error("ERROR on listen");

	printf("Listening on port %u\n", PORT_NO);

	/*
	* main loop: wait for a connection request, echo input line,
	* then close connection.
	*/
	clientlen = sizeof(clientaddr);
	while (1) {
		/*
		 * accept: wait for a connection request
		 */
		childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
		if (childfd < 0)
			error("ERROR on accept");

		/*
		 * gethostbyaddr: determine who sent the message
		 */
		hostaddrp = inet_ntoa(clientaddr.sin_addr);
		if (hostaddrp == NULL)
			error("ERROR on inet_ntoa\n");
		printf("Server established connection with %s\n",
			hostaddrp);

		/*
		 * read: read input string from the client
		 */
		memset(buf, 0, BUFSIZE);
		ssize_t n = read(childfd, buf, BUFSIZE);
		if (n < 0)
			error("ERROR reading from socket");
		printf("Server received %ld bytes: %s", (long)n, buf);

		/*
		 * write: echo the input string back to the client
		 */
		n = write(childfd, buf, strlen(buf));
		if (n < 0)
			error("ERROR writing to socket");

		close(childfd);
	}
}
