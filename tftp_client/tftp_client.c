#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>

#define RRQ	1
#define WRQ	2
#define DATA	3
#define ACK	4
#define ERR	5

#define MAX_FILE_NAME	255
#define PORT	69
#define DATA_SIZE	512
#define TIMEOUT 5

struct tftp_packet{
	unsigned short opcode;
	char* filename;
	char* mode;
};

struct data_packet{
	unsigned short opcode;
	unsigned short block;
	unsigned char data[DATA_SIZE];
};

struct ack_packet{
	unsigned short opcode;
	unsigned short block;
};

struct error_packet{
	unsigned short opcode;
	unsigned short error_code;
	char *error_msg;
};

char *error_message[]={
	"undefined error",
	"file not found",
	"access violation",
	"disk full or allocation error",
	"illegal TFTP operation",
	"Unknown transfer ID",
	"file already exist",
	"no such user"
};

void tftp_timer(int sockfd)
{
	int ready;
	struct timeval timeout;
	fd_set readfs;
	
	/*set timeout to 5 second*/
	timeout.tv_sec = TIMEOUT;
	timeout.tv_usec = 0;

	FD_ZERO(&readfs);
	FD_SET(sockfd, &readfs);

	ready = select(sockfd+1, &readfs, NULL, NULL, &timeout);

	if (ready==-1)
	{
		perror("ERROR in select");
	}
	else if (ready==0)
	{
		printf("TIMEOUT wating data\n");
		exit(0);
	}
}

void put_file(char* filename, int sockfd, struct sockaddr_in server_addr, socklen_t server_len)
{
	FILE *file;
	unsigned short block_num=1;
	unsigned short receive_block_num;
	int data_len;

	struct data_packet data_pack;
	struct ack_packet ack;

	file = fopen(filename, "rb");
	if (file == NULL)
	{
		fprintf(stderr, "ERROR opening file\n");
		close(sockfd);
		exit(0);
	}
	/* receive block num 0 if work successfully*/
	recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, &server_len);

	/* show error when receive error packet from server */
	if (ntohs(ack.opcode)==ERR)
	{
		printf("error: %s\n",error_message[ntohs(ack.block)]);
		exit(0);
	}
		

	while(1)
	{
		/* read data for 512byte to transfer */
		data_len = fread(data_pack.data, 1, DATA_SIZE, file);

		data_pack.opcode = htons(DATA);
		data_pack.block = htons(block_num);
		
		/* send last packet if data is less than 512 */	
		if (data_len < DATA_SIZE)
		{
			sendto(sockfd, &data_pack, sizeof(data_pack)-(DATA_SIZE-data_len), 0, (struct sockaddr *)&server_addr, server_len);
			break;
		}

		sendto(sockfd, &data_pack, DATA_SIZE+4, 0, (struct sockaddr *)&server_addr, server_len);
		
		/* packet send timer start, if took more than 5 senconds, end it */
		tftp_timer(sockfd);

		recvfrom(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, &server_len);

		/* check error and sequence of packet */
		if(ntohs(ack.opcode)!=ACK || ntohs(ack.block)!=block_num)
		{
			printf("received invalid ACK for block %d\n", block_num);
			break;
		}
		block_num++;
	}

	fclose(file);
	printf("file transfer complete\n");
}

void get_file(char* filename, int sockfd, struct sockaddr_in server_addr, socklen_t server_len)
{
	FILE *file;
	struct ack_packet ack;
	struct data_packet data_pack;
	unsigned short block_num=1;
	unsigned short receive_block_num;
	int data_len;

	file = fopen(filename, "wb");
	if (file == NULL)
		perror("ERROR opening file");


	while(1)
	{
		tftp_timer(sockfd);

		data_len = recvfrom(sockfd, &data_pack, sizeof(data_pack), 0, (struct sockaddr *)&server_addr, &server_len);

		if (ntohs(data_pack.opcode)==DATA)
		{
			if (ntohs(data_pack.block) == block_num)
			{
				fwrite(&data_pack.data, 1, DATA_SIZE, file);
				ack.opcode = htons(ACK);
				ack.block = data_pack.block;
				sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, server_len);
				block_num++;
				if(data_len!=DATA_SIZE+4)
				{
					break;
				}
			}
		}
	}
	fclose(file);
	printf("file receive complete\n");
}

int get_packet_len(struct tftp_packet packet)
{
	int length = 0;
	length+=strlen(packet.filename);
	length+=strlen(packet.mode);
	length+=sizeof(packet.opcode);
	length+=2;
	return length;
}

void send_request(int sockfd, struct tftp_packet request_packet, struct sockaddr_in server_addr, socklen_t server_len)
{
	int req_len = get_packet_len(request_packet);
	char *packet = (char*)malloc(req_len);

	memcpy(packet, &request_packet.opcode, sizeof(request_packet.opcode));
	memcpy(packet + sizeof(request_packet.opcode),request_packet.filename,strlen(request_packet.filename)+1);
	memcpy(packet + sizeof(request_packet.opcode)+strlen(request_packet.filename)+1,request_packet.mode,strlen(request_packet.mode)+1);

	if (sendto(sockfd, packet, req_len, 0, (struct sockaddr *)&server_addr, server_len) <0)
		perror("ERROR sending request");
	free(packet);
}

int main (int argc, char* argv[])
{
	char *host = argv[1];
	char *operation = argv[2];
	char *filename = argv[3];
	struct tftp_packet request_packet;

	struct sockaddr_in server_addr;
	socklen_t server_len = sizeof(server_addr);
	int sockfd;
	int req_len;

	if (argc<4)
	{
		fprintf(stderr, "usage : %s <hostname> <get|put> <filename>\n",argv[0]);
		exit(0);
	}

	/* make packet for RRQ/WRQ request */
	sockfd = socket(AF_INET, SOCK_DGRAM,0);
	if (sockfd < 0)
		perror("ERROR opening socket");

	memset((char *)&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	/* use port number 69 as default*/
	server_addr.sin_port = htons(PORT);

	if (inet_aton(host, &server_addr.sin_addr)==0)
		perror("error invalid server address");

	if (strcmp(operation, "get")==0)
	{
		request_packet.opcode = htons(RRQ);
	}
	else if (strcmp(operation, "put")==0)
	{
		request_packet.opcode = htons(WRQ);
	}
	else 
	{
		fprintf(stderr, "invalid operation: use 'get' or 'put'\n");
		close(sockfd);
		exit(0);
	}

	request_packet.filename = filename;
	request_packet.mode = "octet";

	/* send RRQ or WRQ request depends on cmd*/
	send_request(sockfd, request_packet, server_addr, server_len);

	if (strcmp(operation, "get")==0)
	{
		get_file(filename, sockfd, server_addr, server_len);
	}
	else if (strcmp(operation, "put")==0)
	{
		put_file(filename, sockfd, server_addr, server_len);
	}

	close(sockfd);
	return 0;
}
