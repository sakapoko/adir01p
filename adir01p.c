#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <libusb-1.0/libusb.h>

#define VENDOR_ID  0x22ea
#define PRODUCT_ID 0x003a
#define EP_4_IN  0x84
#define EP_4_OUT 0x04
#define PACKET_SIZE 64
#define USB_TIMEOUT 0
#define RECEIVE_TIMEOUT 5

int verbose = 0;
int frequency = 38000;
unsigned char irdata[9600];
int irdata_size = 0;
volatile sig_atomic_t receiving = 0;

struct {
  libusb_context *context;
  struct libusb_device_handle *device;
  unsigned char buf[64];
} remocon;

void dump_data(unsigned char *buf, int len) {
  for (int i = 0; i < len; i++) {
    printf("%02x ", buf[i]);
    if ((i + 1) % 16 == 0) {
      printf("\n");
    }
  }
  printf("\n");
}

int open_remocon() {
  libusb_device *dev;
  libusb_device **devs;
  int i = 0;

  remocon.device = NULL;
  remocon.context = NULL;

  if (libusb_init(&remocon.context) < 0) {
    perror("libusb_init\n");
    return -1;
  }
  libusb_set_debug(remocon.context, 3);
  if ((libusb_get_device_list(remocon.context, &devs)) < 0) {
    perror("no usb device found");
    return -1;
  }
  while ((dev = devs[i++]) != NULL) {
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) < 0) {
      perror("failed to get device descriptor\n");
    }
    /* open first device */
    if (desc.idVendor == VENDOR_ID && desc.idProduct == PRODUCT_ID) {
      libusb_open(dev, &remocon.device);
      break;
    }
  }
  libusb_free_device_list(devs, 1);
  if (remocon.device == NULL) {
    fprintf(stderr, "device not found\n");
    return -1;
  }
  if (libusb_kernel_driver_active(remocon.device, 3) == 1) {
    if (libusb_detach_kernel_driver(remocon.device, 3) != 0) {
      perror("detaching kernel driver failed");
      return -1;
    }
  }
  if (libusb_claim_interface(remocon.device, 3) < 0) {
    perror("claim interface failed");
    return -1;
  }
  return 0;
}

unsigned char *send_remocon(unsigned char *cmd, int len) {
  int size = 0;

  memset(remocon.buf, 0xff, sizeof(remocon.buf));
  memcpy(remocon.buf, cmd, len);
  if (verbose) {
    printf("write data\n");
    dump_data(remocon.buf, PACKET_SIZE);
  }
  if (libusb_interrupt_transfer(remocon.device, EP_4_OUT, remocon.buf, PACKET_SIZE, &size, USB_TIMEOUT) < 0) {
    perror("usb write");
    return NULL;
  }
  memset(remocon.buf, 0x00, PACKET_SIZE);
  if (libusb_interrupt_transfer(remocon.device, EP_4_IN, remocon.buf, PACKET_SIZE, &size, USB_TIMEOUT) < 0) {
    perror("usb read");
    return NULL;
  }
  if (verbose) {
    printf("read data\n");
    dump_data(remocon.buf, PACKET_SIZE);
  }
  if (cmd[0] != remocon.buf[0]) {
    fprintf(stderr, "response error\n");
    return NULL;
  }
  return remocon.buf;
}


void close_remocon() {
  libusb_close(remocon.device);
  libusb_exit(remocon.context);
}

int command_version()
{
  unsigned char cmd[1];
  unsigned char *ret;

  cmd[0] = 0x56;
  if (ret = send_remocon(cmd, 1)) {
    printf("version=%5s\n", ret + 1);
  }
  return 0;
}

int command_receive_start()
{
  unsigned char cmd[8];
  unsigned char *ret;

  cmd[0] = 0x31;
  cmd[1] = (frequency >> 8) & 0xff;
  cmd[2] = frequency & 0xff;
  cmd[3] = 0; // 読込停止フラグ 
  cmd[4] = 0; // 読込停止ON時間MSB
  cmd[5] = 0; // 読込停止ON時間LSB
  cmd[6] = 0; // 読込停止OFF時間MSB
  cmd[7] = 0; // 読込停止OFF時間LSB
  if (ret = send_remocon(cmd, 8)) {
    return 0;
  }
  return -1;
}

int command_receive_stop()
{
  unsigned char cmd[1];
  unsigned char *ret;

  cmd[0] = 0x32;
  if (ret = send_remocon(cmd, 1)) {
    return 0;
  }
  return -1;
}

void command_receive_result()
{
  unsigned char cmd[1];
  unsigned char *ret;
  int size,total,pos;

  irdata_size = 0;
  do {
    cmd[0] = 0x33;
    if (ret = send_remocon(cmd, 1)) {
      size = ret[5];
      total = (ret[1] << 8) + ret[2];
      pos = (ret[3] << 8) + ret[4];
      memcpy(irdata + irdata_size, ret + 6, size * 4);
      irdata_size += size * 4;
    } else {
      return;
    }
  } while (total > 0 && total > pos + size && size > 0);
}

int command_receive_sample()
{
  unsigned char cmd[1];
  unsigned char *ret;
  int size,total,pos;

  irdata_size = 0;
  do {
    cmd[0] = 0x37;
    if (ret = send_remocon(cmd, 1)) {
      total = (ret[1] << 8) + ret[2];
      if (total == 0)
        break;
      size = ret[5];
      pos = (ret[3] << 8) + ret[4];
      memcpy(irdata + irdata_size, ret + 6, size * 4);
      irdata_size += size * 4;
    } else {
      return -1;
    }
  } while (total > 0 && total > pos + size && size > 0);
  return total;
}

int command_set_data()
{
  unsigned char cmd[PACKET_SIZE];
  unsigned char *ret;
  int i;
  int total, pos = 0, size;

  if (verbose) {
    printf("frequency: %d\n", frequency);
    printf("data size: %d\n", irdata_size);
    printf("data: ");
    for (i = 0; i < irdata_size; i++) {
      printf("%02x", irdata[i]);
    }
    printf("\n");
  }
  if (irdata_size == 0) return -1;
  total = irdata_size / 4;
  do {
    cmd[0] = 0x34;
    cmd[1] = (total >> 8) & 0xff;
    cmd[2] = total & 0xff;
    cmd[3] = (pos >> 8) & 0xff;
    cmd[4] = pos & 0xff;
    size = total - pos;
    size = size < 0x0e ? size : 0x0e;
    cmd[5] = size;
    memcpy(cmd + 6, irdata + pos * 4, size * 4);
    pos += size;
    if ((ret = send_remocon(cmd, 6 + size * 4)) == NULL) {
      return -1;
    }
  } while (total > pos);
  return 0;
}

int command_send()
{
  unsigned char cmd[5], *ret;
  cmd[0] = 0x35;
  cmd[1] = (frequency >> 8) & 0xff;
  cmd[2] = frequency & 0xff;
  cmd[3] = (irdata_size / 4 >> 8) & 0xff;
  cmd[4] = irdata_size / 4 & 0xff;
  if (ret = send_remocon(cmd, 5)) {
    return 0;
  }
  return -1;
}

int read_datafile(char *file)
{
  char buf[12000];
  FILE *fp;

  irdata_size = 0;
  if (file[0] == '-') {
    fp = stdin;
  } else if ((fp = fopen(file, "r")) == NULL) {
    return -1;
  }
  while (fgets(buf, 12000, fp)) {
    if (!strncmp(buf, "frequency=", 10)) {
      frequency = atoi(buf + 10);
    } else if (!strncmp(buf, "size=", 5)) {
      irdata_size = atoi(buf + 5);
    } else if (!strncmp(buf, "data=", 5)) {
      for (unsigned int i = 0; i < strlen(buf); i+=2) {
        unsigned int tmp;
        sscanf((const char*)buf + 5 + i, "%02x", &tmp);
        irdata[i / 2] = tmp & 0xff;
      }
      if (irdata_size == 0)
        irdata_size = strlen(buf) / 2;
    }
  }
  fclose(fp);
  return 0;
}

void print_result()
{
  printf("frequency=%d\n", frequency);
  printf("size=%d\n", irdata_size);
  printf("data=");
  for (int i = 0; i < irdata_size; i++) {
    printf("%02x", irdata[i]);
  }
  printf("\n");
}

void alarm_handler(int signum)
{
  fprintf(stderr, "receive timeout\n");
  receiving = -1;
}

int wait_received()
{
  receiving = 1;
  signal(SIGALRM, alarm_handler);
  alarm(RECEIVE_TIMEOUT);
  do {
    int len = command_receive_sample();
    if (len > 0) {
      if (verbose) {
        print_result();
      }
      if ((irdata[irdata_size - 2] == 0x7f) && (irdata[irdata_size - 1] == 0xff)) {
        receiving = 0;
      }
    } else if (len < 0) {
      fprintf(stderr, "receive error\n");
      return -1;
    }
  } while (receiving > 0);
  alarm(0);
  return receiving;
}

int main(int argc, char *argv[]) {
  int opt;
  char file[1024];
  int mode = 0;
  int display_version = 0;

  while ((opt = getopt(argc, argv, "vdrt:f:")) != -1) {
    switch(opt) {
      case 'v':
        display_version = 1;
        break;
      case 'd':
        verbose = 1;
        break;
      case 'r':
        mode = 1;
        break;
      case 't':
        mode = 2;
        strcpy(file, optarg);
        break;
      case 'f':
        frequency = atoi(optarg);
        break;
      default:
        exit(1);
    }
  }

  if (open_remocon() < 0) {
    exit(1);
  }
  if (display_version)
    command_version();
  switch (mode) {
    case 1:
      if (command_receive_start() == 0) {
        if (wait_received() < 0)
          exit(1);
        if (command_receive_stop() == 0) {
          command_receive_result();
          print_result();
        }
      }
      break;
    case 2:
      if (read_datafile(file) == 0)
        if (command_set_data() == 0)
          command_send();
      break;
    default:
      break;
  }
  close_remocon();
  return 0;
}
