// data come from stdin (from arecord -f dat -t raw -D hw:CARD=H2n,DEV=0 - )
// data are supposed to be 48kHz stereo S16 sample, so 32 bits (Digital Audio Tape)
// and when a TCP connection occurs
// the TCP client is fed with data at the offset configured for its specific TCP port
// after having pushed a WAVE header for an infinite 48kHz S16LE stereo file

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <linux/limits.h>
#include <sys/time.h>

int listenSocket(struct in_addr *address, unsigned short port){
	struct sockaddr_in local_sock_addr = {.sin_family = AF_INET, .sin_addr = *address, .sin_port = port};
	int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	// fprintf(stderr, "listen_socket=%d" "\n", listen_socket);
	const int enable = 1;
	if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0){
		// fprintf(stderr,"setsockopt(SO_REUSEADDR) failed" "\n");
	}else{
		int error = bind(listen_socket, (struct sockaddr *)&local_sock_addr, sizeof(local_sock_addr));
		if(error){
			close(listen_socket);
			listen_socket = -1;
		}
	}
	if(listen_socket >= 0){
		int error = listen(listen_socket, 1);
		if(error){
			close(listen_socket);
			listen_socket = -2;
		}
	}
	return listen_socket;
}

#define SAMPLE_RATE (48000)
#define OFFSET_MAX_SECONDS (10)
#define OFFSET_MAX_SAMPLES (SAMPLE_RATE * OFFSET_MAX_SECONDS)

#define FIFO_SAMPLE_SIZE (OFFSET_MAX_SAMPLES) // OFFSET_MAX_SECONDS stereo S16 sample at SAMPLE_RATE
#define MAX_OUTPUTS (16)

typedef struct {
	int fd;
	int offset;
	int remainingOffset;
	int waveHeaderSent;
} Output_s;

typedef struct {
	int index;
	Output_s outputs[MAX_OUTPUTS];
	int32_t inputFifo[FIFO_SAMPLE_SIZE];
	ssize_t inputFifoWriteIndex;
} parserContext_s;

static void contextInitialize(parserContext_s *context){
	context->index = 0;
	int i = MAX_OUTPUTS;
	while(i--){
		context->outputs[i].fd = -1;
		context->outputs[i].offset = 0;
		context->outputs[i].remainingOffset = 0;
	}
	context->inputFifoWriteIndex = 0;
}

static int contextFirstSlotAvailable(parserContext_s *context){
	int i = 0;
	while(i < MAX_OUTPUTS){
		if(-1 == context->outputs[i].fd){
			return(i);
		}
		i++;
	}
	return(-1);
}

int wrap(int newFifoIndex, int fifoSize){
	int returnValue = newFifoIndex;
	if(newFifoIndex < 0){
		returnValue += fifoSize;
	}else if(newFifoIndex >= fifoSize){
		returnValue -= fifoSize;
	}
	// fprintf(stderr, "%s(%d, %d)=>%d" "\n", __func__, newFifoIndex, fifoSize, returnValue);
	return(returnValue);
}

struct {
    char chunkId[4];                    // "RIFF"
    uint32_t chunkSize;
    char format[4];                     // "WAVE"

    char subChunkId1[4];                // "fmt "
    uint32_t subChunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;

    char subChunkId2[4];                // "data"
    uint32_t subChunk2Size;
} wave_header;

static void analyze_and_forward(parserContext_s *context, const uint32_t *buffer, ssize_t sampleCount){
	// fprintf(stderr, "%s(sampleCount=%i)" "\n", __func__, sampleCount);
	int i = sampleCount;
	const uint32_t *p = buffer;
	while(i--){
		context->inputFifo[context->inputFifoWriteIndex] = *p++;
		context->inputFifoWriteIndex = wrap(context->inputFifoWriteIndex + 1, FIFO_SAMPLE_SIZE);
	}
	// We have sampleCount new sample in the FIFO
	// We need to output as much,
	// but only for outputs that have "filled" their offset
	i = MAX_OUTPUTS;
	while(i--){
		Output_s *output = &(context->outputs[i]);
		int toSend = 0;

		if(output->fd != -1){
			if(output->offset){
				if(output->remainingOffset > 0){
					if(sampleCount <= output->remainingOffset){
						output->remainingOffset -= sampleCount;
					}else{
						toSend = sampleCount - output->remainingOffset;
						output->remainingOffset = 0;
					}
				}else{
					toSend = sampleCount;
				}
			}else{
				toSend = sampleCount;
			}
		}
		if(toSend > 0){
			if(0 == output->waveHeaderSent){
				output->waveHeaderSent = 1;
				write(output->fd, &wave_header, sizeof(wave_header));
			}
			int startReadIndex = wrap(context->inputFifoWriteIndex - toSend, FIFO_SAMPLE_SIZE);
			if(startReadIndex > context->inputFifoWriteIndex){
				// 2 write() from the FIFO required
				int firstWriteLength = FIFO_SAMPLE_SIZE - startReadIndex; // up-to the end of the FIFO
				int secondWriteLength = toSend - firstWriteLength;        // remaining sample(s) from the start of the FIFO
				int written1 = write(output->fd, context->inputFifo + startReadIndex, firstWriteLength * sizeof(uint32_t));
				int written2 = write(output->fd, context->inputFifo, secondWriteLength * sizeof(uint32_t));
				// fprintf(stderr, "%s:written1=%i/%i, written2=%i/%i" "\n", __func__, written1 / sizeof(uint32_t), firstWriteLength, written2 / sizeof(uint32_t), secondWriteLength);

			}else{
				// One single write() from the FIFO is enough
				int written = write(output->fd, context->inputFifo + startReadIndex, toSend * sizeof(uint32_t));
				// fprintf(stderr, "%s:written=%i" "\n", __func__, written / sizeof(uint32_t));
				if(written != toSend * sizeof(uint32_t)){
					perror("fwrite()");
				}
			}
		}
	}
}

#define MAX_LISTENING_SOCKETS (16)

int startsWith(const char *start, const char *with){
	return(start == strstr(start, with));
}
	
int main(int argc, const char *argv[]){
	if(argc < 2){
		fprintf(stderr,
				"Usage: %s <port definition> [<port definition> [ .... ]]" "\n"
				"with <port definition> either a TCP port number or stdout," 
			        "optionally followed by a ':' and an integer sample offset" "\n",
				argv[0]);
		exit(1);
	}
	// Initialize WAVE header for an infinite 48kHz S16LE stereo stream
	memcpy(&wave_header.chunkId,     "RIFF", 4);
	wave_header.chunkSize = 0xFFFFFFF8;
	memcpy(&wave_header.format,      "WAVE", 4);
	memcpy(&wave_header.subChunkId1, "fmt ", 4);
	wave_header.subChunk1Size = 16;
	wave_header.audioFormat = 1;
	wave_header.numChannels = 2;
	wave_header.sampleRate = SAMPLE_RATE;
	wave_header.byteRate = SAMPLE_RATE * 4;
	wave_header.blockAlign = 4;
	wave_header.bitsPerSample = 16;
	memcpy(&wave_header.subChunkId2, "data", 4);
	wave_header.subChunk2Size = 0xFFFFFFD4;

	int in  = STDIN_FILENO;
	{
		parserContext_s context;
		contextInitialize(&context);
		fprintf(stderr, "sizeof(context) = %i" "\n", sizeof(context));
		int listeningSockets[MAX_LISTENING_SOCKETS];
		int socketOffset[MAX_LISTENING_SOCKETS];
		int listeningSocketCount = 0;
		for(int i = 0 ; i < MAX_LISTENING_SOCKETS ; i++){
			listeningSockets[i] = -1;
			socketOffset[i] = 0;
		}
		struct in_addr listenAddress = {0};
		for(int i = 1 ; i < MAX_LISTENING_SOCKETS ; i++){
			if(i < argc){
				int offset = 0;
				if(startsWith(argv[i], "stdout")){
					const char *colon = strchr(argv[i], ':');
					if(colon){
						offset = atoi(colon + 1);
					} 
					int index  = contextFirstSlotAvailable(&context);
					if(index != -1){
						context.outputs[index].fd = STDOUT_FILENO;
						context.outputs[index].offset = offset;
						context.outputs[index].remainingOffset = offset;
						context.outputs[index].waveHeaderSent = 0;
						fprintf(stderr, "Outputting to stdout with offset %i" "\n", offset);
					}
				}else{
					int tcpPort = atoi(argv[i]);
					if((0 < tcpPort) && (tcpPort < 65535)){
						const char *colon = strchr(argv[i], ':');
						if(colon){
							offset = atoi(colon + 1);
						} 
						if(offset < OFFSET_MAX_SAMPLES){
							socketOffset[i] = offset;
						}else{
							socketOffset[i] = 0;
						}
						listeningSockets[i] = listenSocket(&listenAddress, htons(tcpPort));
						fprintf(stderr, "Listening to TCP port %i, with offset %i" "\n",tcpPort, offset);
					}
				}
			}
		}
						
		for(;;){
			void updateMax(int *m, int n){
				int max = *m;
				if(n > max){
					*m = n;
				}
			}
			int max = -1;
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds); updateMax(&max, STDIN_FILENO);
			for(int i = 0 ; i < MAX_LISTENING_SOCKETS ; i++){
				int listeningSocket = listeningSockets[i];
				if(-1 != listeningSocket && (0 <= contextFirstSlotAvailable(&context))){
					FD_SET(listeningSocket, &fds); updateMax(&max, listeningSocket);
				}
			}
			int i = MAX_OUTPUTS;
			while(i--){
				int fd = context.outputs[i].fd;
				if(-1 != fd){
					FD_SET(fd, &fds); updateMax(&max, fd);
				}
			}
			struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
			int selected = select(max + 1, &fds, NULL, NULL, &timeout);
			if(selected > 0){
				// Check forwarding socket for error (read ready should not happen)
				i = MAX_OUTPUTS;
				while(i--){
					int fd = context.outputs[i].fd;
					if((-1 != fd) && (STDOUT_FILENO != fd) && FD_ISSET(fd, &fds)){
						close(fd);
						context.outputs[i].fd = -1;
						fprintf(stderr, "slot %d had an error, closing fd %d" "\n", i, fd);
					}
				}
				// Check listening sockets for incoming connection
				for(int i = 0 ; i < MAX_LISTENING_SOCKETS ; i++){
					int listeningSocket = listeningSockets[i];
					if(-1 != listeningSocket && FD_ISSET(listeningSocket, &fds)){
						int index = contextFirstSlotAvailable(&context);
						if(-1 != index){
							context.outputs[index].fd = accept(listeningSocket, NULL, NULL);
							context.outputs[index].offset = socketOffset[i];
							context.outputs[index].remainingOffset = socketOffset[i];
							fprintf(stderr, "accepted connexion to slot %d (fd=%d), offset=%i sample(s)" "\n", index, context.outputs[index].fd, context.outputs[index].offset);
						}
					}
				}
				if(FD_ISSET(STDIN_FILENO, &fds)){
					int32_t buffer[SAMPLE_RATE / 10];
					// fprintf(stderr, "data available on stdin" "\n");
					ssize_t lus = fread(buffer, sizeof(int32_t), sizeof(buffer) / sizeof(int32_t), stdin);
					if(lus <= 0){
						break;
					}
					analyze_and_forward(&context, buffer, lus);
				}
				fflush(stdout);
			}
		}
	}
	return(0);
}


