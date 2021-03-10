#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <getopt.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// "Ok ok, use them then..."
#define SOCKET_CMD_DMA         0xFF01
#define SOCKET_CMD_DMARUN      0xFF02
#define SOCKET_CMD_KEYB        0xFF03
#define SOCKET_CMD_RESET       0xFF04
#define SOCKET_CMD_WAIT        0xFF05
#define SOCKET_CMD_DMAWRITE    0xFF06
#define SOCKET_CMD_REUWRITE    0xFF07
#define SOCKET_CMD_KERNALWRITE 0xFF08
#define SOCKET_CMD_DMAJUMP     0xFF09
#define SOCKET_CMD_MOUNT_IMG   0xFF0A
#define SOCKET_CMD_RUN_IMG     0xFF0B

// Only available on U64
#define SOCKET_CMD_VICSTREAM_ON    0xFF20
#define SOCKET_CMD_AUDIOSTREAM_ON  0xFF21
#define SOCKET_CMD_DEBUGSTREAM_ON  0xFF22
#define SOCKET_CMD_VICSTREAM_OFF   0xFF30
#define SOCKET_CMD_AUDIOSTREAM_OFF 0xFF31
#define SOCKET_CMD_DEBUGSTREAM_OFF 0xFF32

#define MAX_STRING_SIZE 128
#define COMMAND_PORT 64

typedef struct {
    char hostname[MAX_STRING_SIZE];
    FILE *dmafile_fp;
    char dmafilename[MAX_STRING_SIZE];
    uint8_t *dmafile_address;
    int dmafile_size;
    int do_reset;
    int do_loadprg;
    int socket;
} program_data;

void close_socket(program_data *data)
{
    if (data->socket) {
        shutdown(data->socket, SHUT_RDWR);
        close(data->socket);
        data->socket = 0;
    }
}

int open_socket(program_data *data)
{
    struct addrinfo hints;
    struct addrinfo *servinfo = NULL;
    struct addrinfo *p = NULL;
    int result = 0;
    char port[MAX_STRING_SIZE] = {0};

    snprintf(port, MAX_STRING_SIZE - 1, "%d", COMMAND_PORT);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((result = getaddrinfo(data->hostname, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return EXIT_FAILURE;
    }

    for (p=servinfo; p != NULL; p = p->ai_next) {
        if ((data->socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }

        if (connect(data->socket, p->ai_addr, p->ai_addrlen) == -1) {
            close(data->socket);
            continue;
        }

        // Connected succesfully!
        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "Couldn't connect to Ultimate64\n.");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int open_file(program_data *data)
{
    struct stat st;

    data->dmafile_fp = fopen(data->dmafilename, "r");
    if (!data->dmafile_fp) {
        perror("Error");
        return EXIT_FAILURE;
    }

	if (fstat(fileno(data->dmafile_fp), &st) != 0) {
		perror("Error");
		fclose(data->dmafile_fp);
		return EXIT_FAILURE;
	}

	data->dmafile_size = (int)st.st_size;

	data->dmafile_address = mmap(NULL, data->dmafile_size, PROT_READ, MAP_SHARED, fileno(data->dmafile_fp), 0);
	if (data->dmafile_address == MAP_FAILED) {
		perror("Error");
		fclose(data->dmafile_fp);
		return EXIT_FAILURE;
	}

    return EXIT_SUCCESS;
}

void close_file(program_data *data)
{
    if (data->dmafile_address) {
        munmap(data->dmafile_address, data->dmafile_size);
        data->dmafile_address = NULL;
    }

    if (data->dmafile_fp) {
        fclose(data->dmafile_fp);
        data->dmafile_fp = NULL;
        data->dmafile_size = 0;
    }
}

int reset(program_data *data)
{
    uint16_t reset_data[] = {
        htole16(SOCKET_CMD_RESET),
        0x0000
    };

    size_t size = sizeof(reset_data) / sizeof(reset_data[0]);

    if (open_socket(data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < size; i++) {
        if (send(data->socket, &reset_data[i], size, 0) == -1) {
            perror("Error");
            close_socket(data);
            return EXIT_FAILURE;
        }
    }

    close_socket(data);

    return EXIT_SUCCESS;
}

int load_prg(program_data* data)
{
    uint16_t loadprg_data[] = {
        htole16(SOCKET_CMD_DMARUN),
        htole16(data->dmafile_size)
    };

    size_t data_size = sizeof(loadprg_data) / sizeof(loadprg_data[0]);

    if (open_socket(data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    // Send load&run command and size
    for (size_t i = 0; i < data_size; i++) {
        if (send(data->socket, &loadprg_data[i], data_size, 0) == -1) {
            perror("Error");
            close_socket(data);
            return EXIT_FAILURE;
        }
    }

    // Send actual file
    if (send(data->socket, data->dmafile_address, data->dmafile_size, 0) == -1) {
        perror("Error");
        close_socket(data);
        return EXIT_FAILURE;
    }

    close_socket(data);

    return EXIT_SUCCESS;
}

void print_help(char *program_name)
{
    fprintf(stderr, "Usage: %s [-i ip address] [-r] [-p PRG name] [-h]\n", program_name);
    fprintf(stderr, "   -i IP address   Ultimate 64 IP address\n");
    fprintf(stderr, "   -r              Reset Ultimate 64\n");
    fprintf(stderr, "   -p PRG name     DMA load and run program\n");
    fprintf(stderr, "   -h              Print this help text\n");
}

int parse_arguments(int argc, char **argv, program_data *data)
{
    int c = 0;
    char *program_name = argv[0];

    opterr = 0;
    errno = 0;

    while ((c = getopt(argc, argv, "i:hrp:")) != -1) {
        switch (c) {
            case 'i':
                strncpy(data->hostname, optarg, MAX_STRING_SIZE - 1);
                break;
            case 'r':
                data->do_reset = 1;
                break;
            case 'p':
                strncpy(data->dmafilename, optarg, MAX_STRING_SIZE - 1);
                if (open_file(data) != EXIT_SUCCESS) {
                    return EXIT_FAILURE;
                }
                data->do_loadprg = 1;
                break;
            case '?':
                if (optopt == 'i' || optopt == 'p') {
                    fprintf(stderr, "Option -%c requires an argument\n", optopt);
                } else {
                    fprintf(stderr, "Invalid argument '-%c'\n", optopt);
                }
                return EXIT_FAILURE;
            case 'h':
                /* fall through */
            default:
                print_help(program_name);
                return EXIT_FAILURE;
        }
    }

    if (!strlen(data->hostname)) {
        fprintf(stderr, "-i IP address is mandatory\n");
        return EXIT_FAILURE;
    }

    if (data->do_reset && strlen(data->dmafilename)) {
        fprintf(stderr, "Can't reset and load/run at the same time\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    program_data data;
    memset(&data, 0, sizeof(program_data));

    if (parse_arguments(argc, argv, &data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    if (data.do_reset) {
        reset(&data);
    }

    if (data.do_loadprg) {
        load_prg(&data);
    }

    return EXIT_SUCCESS;
}
