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

//#define DEBUG

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
// Exists only on my fork of 1541ultimate
#define SOCKET_CMD_POWEROFF    0xFF0C

// Only available on U64
#define SOCKET_CMD_VICSTREAM_ON    0xFF20
#define SOCKET_CMD_AUDIOSTREAM_ON  0xFF21
#define SOCKET_CMD_DEBUGSTREAM_ON  0xFF22
#define SOCKET_CMD_VICSTREAM_OFF   0xFF30
#define SOCKET_CMD_AUDIOSTREAM_OFF 0xFF31
#define SOCKET_CMD_DEBUGSTREAM_OFF 0xFF32

// Undocumented, shall only be used by developers.
#define SOCKET_CMD_LOADSIDCRT   0xFF71
#define SOCKET_CMD_LOADBOOTCRT  0xFF72
#define SOCKET_CMD_READFLASH    0xFF75
#define SOCKET_CMD_DEBUG_REG    0xFF76

#define MAX_STRING_SIZE 128
#define COMMAND_PORT 64
#define MAX_MEM 65536
#define MAX_D64 200000
#define FTYPE_LEN 3

typedef enum {
    PRG_LOAD,
    PRG_RUN,
    D64_LOAD,
    D64_RUN,
    NUM_OF_FILE_ACTIONS
} file_action;

typedef struct {
    char hostname[MAX_STRING_SIZE];
    FILE *file_fp;
    char filename[MAX_STRING_SIZE];
    uint8_t *file_address;
    size_t file_size;
    int do_reset;
    int do_load;
    int do_run;
    int do_poweroff;
    int do_file;
    int socket;
    file_action f_action;
} program_data;

#if defined(DEBUG)
void dump_hex(const void* data, size_t size)
{
    char ascii[17];
    size_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i) {
        printf("%02X ", ((unsigned char*)data)[i]);
        if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
            ascii[i % 16] = ((unsigned char*)data)[i];
        } else {
            ascii[i % 16] = '.';
        }
        if ((i+1) % 8 == 0 || i+1 == size) {
            printf(" ");
            if ((i+1) % 16 == 0) {
                printf("|  %s \n", ascii);
            } else if (i+1 == size) {
                ascii[(i+1) % 16] = '\0';
                if ((i+1) % 16 <= 8) {
                    printf(" ");
                }
                for (j = (i+1) % 16; j < 16; ++j) {
                    printf("   ");
                }
                printf("|  %s \n", ascii);
            }
        }
    }
}
#endif

void close_socket(program_data *data)
{
#if defined(DEBUG)
    printf("Closing socket: %s:%d\n", data->hostname, COMMAND_PORT);
#endif

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

#if defined(DEBUG)
    printf("Opening socket: %s:%d\n", data->hostname, COMMAND_PORT);
#endif

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

        // Connected successfully!
        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "Couldn't connect to Ultimate 64\n.");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int open_file(program_data *data)
{
    struct stat st;

#if defined(DEBUG)
    printf("Opening file: %s\n", data->filename);
#endif

    data->file_fp = fopen(data->filename, "r");
    if (!data->file_fp) {
        perror("Error");
        return EXIT_FAILURE;
    }

    if (fstat(fileno(data->file_fp), &st) != 0) {
        perror("Error");
        fclose(data->file_fp);
        return EXIT_FAILURE;
    }

    data->file_size = st.st_size;

#if defined(DEBUG)
    printf("File size: %lu\n", data->file_size);
#endif

    data->file_address = mmap(NULL, data->file_size, PROT_READ, MAP_SHARED, fileno(data->file_fp), 0);
    if (data->file_address == MAP_FAILED) {
        perror("Error");
        fclose(data->file_fp);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

void close_file(program_data *data)
{
#if defined(DEBUG)
    printf("Closing file: %s\n", data->filename);
#endif

    if (data->file_address) {
        munmap(data->file_address, data->file_size);
        data->file_address = NULL;
    }

    if (data->file_fp) {
        fclose(data->file_fp);
        data->file_fp = NULL;
        data->file_size = 0;
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

#if defined(DEBUG)
    printf("Sending reset command:\n");
    for (size_t i = 0; i < size; i++) {
        printf("%04x\n", reset_data[i]);
    }
#endif

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

int power_off(program_data *data)
{
    uint16_t poweroff_data[] = {
        htole16(SOCKET_CMD_POWEROFF),
        0x0000
    };

    size_t size = sizeof(poweroff_data) / sizeof(poweroff_data[0]);

    if (open_socket(data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

#if defined(DEBUG)
    printf("Sending power off command:\n");
    for (size_t i = 0; i < size; i++) {
        printf("%04x\n", poweroff_data[i]);
    }
#endif

    for (size_t i = 0; i < size; i++) {
        if (send(data->socket, &poweroff_data[i], size, 0) == -1) {
            perror("Error");
            close_socket(data);
            return EXIT_FAILURE;
        }
    }

    close_socket(data);

    return EXIT_SUCCESS;
}

int prg(program_data* data)
{
    uint16_t runprg_data[] = {
        htole16(SOCKET_CMD_DMARUN),
        htole16(data->file_size)
    };

    uint16_t loadprg_data[] = {
        htole16(SOCKET_CMD_DMA),
        htole16(data->file_size)
    };

    uint16_t *command = NULL;
#if defined(DEBUG)
    char *info = NULL;
#endif

    switch(data->f_action) {
        case PRG_RUN:
            command = (uint16_t *)&runprg_data;
#if defined(DEBUG)
            info = "Running";
#endif
            break;
        case PRG_LOAD:
            command = (uint16_t *)&loadprg_data;
#if defined(DEBUG)
            info = "Loading";
#endif
            break;
        default:
            return EXIT_FAILURE;
    }

    size_t data_size = sizeof(runprg_data) / sizeof(runprg_data[0]);

    if (open_socket(data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

#if defined(DEBUG)
    printf("%s PRG\n", info);
    printf("Sending PRG command:\n");
    for (size_t i = 0; i < data_size; i++) {
        printf("%04x\n", command[i]);
    }
    printf("\nContents:\n");
    dump_hex(data->file_address, data->file_size);
    printf("\n");
#endif

    // Send load&run command and size
    for (size_t i = 0; i < data_size; i++) {
        if (send(data->socket, &command[i], data_size, 0) == -1) {
            perror("Error");
            close_socket(data);
            return EXIT_FAILURE;
        }
    }

    // Send actual file
    if (send(data->socket, data->file_address, data->file_size, 0) == -1) {
        perror("Error");
        close_socket(data);
        return EXIT_FAILURE;
    }

    close_socket(data);

    return EXIT_SUCCESS;
}

int d64(program_data *data)
{
    uint16_t run_command = htole16(SOCKET_CMD_RUN_IMG);
    uint16_t mount_command = htole16(SOCKET_CMD_MOUNT_IMG);
    uint32_t file_len = htole32(data->file_size);

    uint16_t command = 0;
#if defined(DEBUG)
    char *info = NULL;
#endif

    if (open_socket(data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    switch(data->f_action) {
        case D64_LOAD:
            command = mount_command;
#if defined(DEBUG)
            info = "Mounting";
#endif
            break;
        case D64_RUN:
            command = run_command;
#if defined(DEBUG)
            info = "Running";
#endif
            break;
        default:
            return EXIT_FAILURE;
    }

#if defined(DEBUG)
    printf("%s D64\n", info);
    printf("Sending D64 command:\n");
    printf("%04x\n", command);
    printf("%04x\n\n", file_len);
    printf("Contents:\n");
    dump_hex(data->file_address, data->file_size);
    printf("\n");
#endif

    // Command
    if (send(data->socket, &command, sizeof(uint16_t), 0) == -1) {
        perror("Error");
        close_socket(data);
        return EXIT_FAILURE;
    }

    // File length, 3 bytes.
    // Quoting from "1541ultimate/python/sock.py":
    // # Send only 3 of the 4 bytes. Who invented this?!
    if (send(data->socket, &file_len, 3, 0) == -1) {
        perror("Error");
        close_socket(data);
        return EXIT_FAILURE;
    }

    // Send actual file
    if (send(data->socket, data->file_address, data->file_size, 0) == -1) {
        perror("Error");
        close_socket(data);
        return EXIT_FAILURE;
    }

    close_socket(data);

    return EXIT_FAILURE;
}

int load(program_data *data)
{
#if defined(DEBUG)
    printf("Load: %s\n", data->filename);
#endif

    if (open_file(data) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    switch (data->f_action) {
        case PRG_RUN:
            /* fall through */
        case PRG_LOAD:
            if (data->file_size > MAX_MEM) {
                fprintf(stderr, "Error: PRG file too big, %lu bytes\n", data->file_size);
                close_file(data);
                return EXIT_FAILURE;
            }
            if (prg(data) != EXIT_SUCCESS) {
                close_file(data);
                return EXIT_FAILURE;
            }
            break;
        case D64_RUN:
            /* fall through */
        case D64_LOAD:
            if (data->file_size > MAX_D64) {
                fprintf(stderr, "Error: D64 file too big, %lu bytes\n", data->file_size);
                close_file(data);
                return EXIT_FAILURE;
            }
            if (d64(data) != EXIT_SUCCESS) {
                close_file(data);
                return EXIT_FAILURE;
            }
            break;
        default:
            close_file(data);
            return EXIT_FAILURE;
    }

    close_file(data);

    return EXIT_SUCCESS;
}

void print_help(char *program_name)
{
    fprintf(stderr, "Usage: %s [-i IP address] [-l PRG/D64 name] [-r] [-x] [-h]\n", program_name);
    fprintf(stderr, "   -i IP address   Ultimate 64 IP address\n");
    fprintf(stderr, "   -l PRG/D64 file DMA load and run program/image\n");
    fprintf(stderr, "   -m PRG/D64 file DMA load program or mount image\n");
    fprintf(stderr, "   -r              Reset Ultimate 64\n");
    fprintf(stderr, "   -x              Power off Ultimate 64\n");
    fprintf(stderr, "   -h              Print this help text\n");
}

int parse_arguments(int argc, char **argv, program_data *data)
{
    int c = 0;
    char *program_name = argv[0];
    int file_len = 0;

    opterr = 0;
    errno = 0;

    while ((c = getopt(argc, argv, "i:l:m:rxh")) != -1) {
        switch (c) {
            case 'i':
                strncpy(data->hostname, optarg, MAX_STRING_SIZE - 1);
                break;
            case 'l':
                strncpy(data->filename, optarg, MAX_STRING_SIZE - 1);
                data->do_run = 1;
                break;
            case 'm':
                strncpy(data->filename, optarg, MAX_STRING_SIZE - 1);
                data->do_load = 1;
                break;
            case 'r':
                data->do_reset = 1;
                break;
            case 'x':
                data->do_poweroff = 1;
                break;
            case '?':
                if (optopt == 'i' || optopt == 'l' || optopt == 'm') {
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

    if (data->do_reset && (data->do_load || data->do_run)) {
        fprintf(stderr, "Can't reset and load/run at the same time\n");
        return EXIT_FAILURE;
    }

    if (data->do_load || data->do_run) {
        data->do_file = 1;

        file_len = strlen(data->filename);

        // Assume PRG if filename is less than 3 characters.
        if (file_len < FTYPE_LEN) {
            if(data->do_run) {
                data->f_action = PRG_RUN;
            } else {
                data->f_action = PRG_LOAD;
            }
#if defined(DEBUG)
            printf("File type: PRG\n");
#endif

            return EXIT_SUCCESS;
        }

        // Check if filename contains d64/D64
        if ((strncmp(&data->filename[file_len - FTYPE_LEN], "d64", FTYPE_LEN) == 0 ||
                    strncmp(&data->filename[file_len - FTYPE_LEN], "D64", FTYPE_LEN) == 0)) {
            if (data->do_run) {
                data->f_action = D64_RUN;
            } else {
                data->f_action = D64_LOAD;
            }
#if defined(DEBUG)
            printf("File type: D64\n");
#endif
        } else {
            if (data->do_run) {
                data->f_action = PRG_RUN;
            } else {
                data->f_action = PRG_LOAD;
            }
#if defined(DEBUG)
            printf("File type: PRG\n");
#endif
        }
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

    if (data.do_poweroff) {
        return power_off(&data);
    }

    if (data.do_reset) {
        return reset(&data);
    }

    if (data.do_file) {
        return load(&data);
    }

    printf("Ok?\n");
    return EXIT_SUCCESS;
}
