/* A TFTP server.
*
* Adapted from udp example client and server from Marcel which is
* Copyright (c) 2016, Marcel Kyas
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * Neither the name of Reykjavik University nor the
*       names of its contributors may be used to endorse or promote products
*       derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MARCEL
* KYAS NOR REYKJAVIK UNIVERSITY BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
* OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>

#define BLOCK_SIZE 516
#define HEADER_SIZE 4
#define DATA_SIZE 512

const int READ_OP = 1;
const int WRITE_OP = 2;
const int DATA_OP = 3;
const int ACK_OP = 4;
const int ERROR_OP = 5;

const int FILE_ERROR = 1;
const int ACCESS_ERROR = 2;
const int ILLEGALOP_ERROR = 4;

struct rrq {
	short op;
	char filename[DATA_SIZE - 1];
	char mode[1];
};

struct data {
	short op;
	short block_nr;
	char bytes[DATA_SIZE];
};

struct error {
	short op;
	short error_code;
	char err_msg[DATA_SIZE];
};


void print_usage() {
	printf("Usage ./tftpd [PORT] [DATA FOLDER]\n");
}

void print_info(char * filename, in_addr_t sin_addr, in_port_t sin_port) {
	/* Print the ip address elegantly. */
	printf("file \"%s\" requested from %d.%d.%d.%d:%hu\n",
			filename,
			(sin_addr >> 24) & 0xff,
			(sin_addr >> 16) & 0xff,
			(sin_addr >> 8) & 0xff,
			(sin_addr) & 0xff,
			sin_port);
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		print_usage();
		return 0;
	}

	/* Parse the arguments. */
	short requested_port;
	sscanf(argv[1], "%hd", &requested_port);
	char * directory = argv[2];
	char resolved_directory[PATH_MAX];
	long long realret = (long long) realpath(directory, resolved_directory);
	if(realret == 0) {
		perror("error resolving data directory path");
		exit(errno);
	}

	int sockfd;
	struct sockaddr_in server, client;

	/* Create and bind a UDP socket. */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;

	/* Network functions need arguments in network byte order instead
	 * of host byte order. The macros htonl, htons convert the
	 * values.
	 */
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(requested_port);

	ssize_t ret = bind(sockfd, (struct sockaddr *) &server, (socklen_t) sizeof(server));
	if (ret < 0) {
		perror("bind returned an error, terminating");
		exit(errno);
	}

	printf("Server started on port %hu with data folder %s, listening for requests...\n", requested_port, resolved_directory);
	for (;;) {
		struct rrq request;
		memset(&request, 0, sizeof(request));
		socklen_t len = (socklen_t) sizeof(client);

		/* Prepare error struct for later use. */
		struct error err;
		memset(&err, 0, sizeof(err));
		err.op = htons(ERROR_OP);

		/* Listen for connections on the specified port. */
		ret = recvfrom(sockfd, (void *) &request, sizeof(request), 0, (struct sockaddr *) &client, &len);
		/* Check return value of recvfrom which is only unusual if it is < 0. */
		if (ret < 0) {
			perror("recvfrom returned an error, terminating");
			exit(errno);
		}

		/* We dont allow uploading. */
		if(ntohs(request.op) == WRITE_OP) {
			err.error_code = htons(ACCESS_ERROR);
			strncpy(err.err_msg, "This server does not support uploading!", sizeof(err.err_msg));
			ret = sendto(sockfd, &err, strlen(err.err_msg) + HEADER_SIZE, 0, (struct sockaddr *) &client, len);
			if (ret < 0) {
				perror("sendto returned an error when we received a write request");
			}
			continue;
		}

		/* Unexpected first contact */
		if(ntohs(request.op) != READ_OP) {
			err.error_code = htons(ILLEGALOP_ERROR);
			strncpy(err.err_msg, "Didn't receive read request at start of communication!", sizeof(err.err_msg));
			ret = sendto(sockfd, &err, strlen(err.err_msg) + HEADER_SIZE, 0, (struct sockaddr *) &client, len);
			if (ret < 0) {
				perror("sendto returned an error when we received an unknown request");
			}
			continue;
		}

		/* Create the file path ex: data/example_data1 */
		char filepath[PATH_MAX];
		memset(&filepath, 0, sizeof(filepath));
		strcpy(filepath, directory);
		strcat(filepath, "/");
		strcat(filepath, request.filename);
		char resolved_filepath[PATH_MAX];

		/* Using realpath to resolve symlinks and stuff, to prevent
		 * files outside of data directory being transmitted. */
		realret = (long long) realpath(filepath, resolved_filepath);
		if(realret == 0) {
			perror("error resolving path to requested file");
		}
		/* We don't want users requesting ../../../secret/passwords.txt
		 * so we force the path to start with the path to the data directory. */
		if (strncmp(resolved_filepath, resolved_directory, strlen(resolved_directory))) {
			err.error_code = htons(ACCESS_ERROR);
			strncpy(err.err_msg, "You are not authorized to access this file!", sizeof(err.err_msg));
			ret = sendto(sockfd, &err, strlen(err.err_msg) + HEADER_SIZE, 0, (struct sockaddr *) &client, len);
			if (ret < 0) {
				perror("sendto returned an error when we received an unauthorized request");
			}
			continue;
		}

		/* Open the requested file in byte read mode. */
		FILE *file = fopen(resolved_filepath, "rb");
		if (!file) {
			err.error_code = htons(FILE_ERROR);
			strncpy(err.err_msg, "The requested file could not be opened/found!", sizeof(err.err_msg));
			ret = sendto(sockfd, &err, strlen(err.err_msg) + HEADER_SIZE, 0, (struct sockaddr *) &client, len);
			if (ret < 0) {
				perror("sendto returned an error when we received a request for an unkown file");
			}
			continue;
		}

		/* Print information about the request. */
		print_info(request.filename, ntohl(client.sin_addr.s_addr), client.sin_port);

		struct data datablock;
		memset(&datablock, 0, sizeof(datablock));
		struct data ackblock;
		memset(&ackblock, 0, sizeof(ackblock));

		int bytes_read = 0;
		int next_block = 1;
		int read = 1;
		for (;;) {
			datablock.op = htons(DATA_OP);
			datablock.block_nr = htons(next_block);
			/* If the last acknowledgement block had the wrong block_nr or
			 * we received unkown data we send the last block again. */
			if (read) {
				memset(datablock.bytes, 0, DATA_SIZE);
				bytes_read = fread(&datablock.bytes, 1, DATA_SIZE, file);
			}
			read = 0;
			/* Send the datablock either with old failed to send data or fresh meat. */
			ret = sendto(sockfd, &datablock, (size_t) bytes_read + HEADER_SIZE, 0,
			(struct sockaddr *) &client, len);
			if (ret < 0) {
				perror("sendto returned an error when we sent a datablock");
				exit(errno);
			}

			memset(&ackblock, 0, sizeof(ackblock));
			ret = recvfrom(sockfd, (void *) &ackblock, BLOCK_SIZE,
			0, (struct sockaddr *) &client, &len);
			if (ret < 0) {
				perror("recvfrom returned an error when we were trying to receive an acknowledgement block");
				exit(errno);
			}

			/* Make numbers inside received block readable. */
			ackblock.op = ntohs(ackblock.op);
			ackblock.block_nr = ntohs(ackblock.block_nr);

			if (ackblock.op == ACK_OP && ackblock.block_nr == next_block) {
				/* We successfully transmitted a block to the client
				 * so we increment the block counter and set read to true. */
				next_block++;
				read = 1;
				if (bytes_read < DATA_SIZE) {
					/* If we sent a block with less than 512 bytes and it succeeded
					 * we consider the file transfer done. */
					break;
				}
			} else if (ackblock.op == ERROR_OP) {
				struct error * errblock = (struct error *) &ackblock;
				/* Null terminate the err_msg just in case. */
				errblock->err_msg[DATA_SIZE - 1] = '\0';
				printf("ERROR block from client with nr: %d err_code: %d msg: %s\n", next_block, errblock->error_code, errblock->err_msg);
				fflush(stdout);
				break;
			}
		}

		fclose(file);
		printf("%s\n", "file transfer success");
		fflush(stdout);
	}
}
