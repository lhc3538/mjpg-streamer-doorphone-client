/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>

#include <fcntl.h>
#include <linux/soundcard.h>

#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/wait.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "TCP input plugin"

#define BUFLEN 1024
/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;
static pthread_mutex_t controls_mutex;
static int plugin_number;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

int m_rate = 22050,
    m_bits = 16,
    m_channels = 2;

int fd_audio;
int  audio_init(int rate,int size,int channels)
{
    int arg;	// 用于ioctl调用的参数
    int rul = 0;   // 系统调用的返回值
    // 打开声音设备
    fd_audio = open("/dev/dsp", O_RDWR);
    if (fd_audio < 0)
    {
        perror("open of /dev/dsp failed");
        return -1;
    }
    // 设置采样时的量化位数
    arg = size;
    rul = ioctl(fd_audio, SOUND_PCM_WRITE_BITS, &arg);
    if (rul == -1)
        perror("SOUND_PCM_WRITE_BITS ioctl failed");
    if (arg != size)
        perror("unable to set sample size");
    // 设置采样时的声道数目
    arg = channels;
    rul = ioctl(fd_audio, SOUND_PCM_WRITE_CHANNELS, &arg);
    if (rul == -1)
        perror("SOUND_PCM_WRITE_CHANNELS ioctl failed");
    if (arg != channels)
        perror("unable to set number of channels");
    // 设置采样时的采样频率
    arg = rate;
    rul = ioctl(fd_audio, SOUND_PCM_WRITE_RATE, &arg);
    if (rul == -1)
        perror("SOUND_PCM_WRITE_WRITE ioctl failed");
    return rul;
}
int audio_read(unsigned char *databuf)
{
    int rul;
    rul = read(fd_audio, databuf, BUFLEN); // 录音
    if (rul != BUFLEN)
        perror("read wrong number of bytes");
    return rul;
}
/*** plugin interface functions ***/

/******************************************************************************
Description.: parse input parameters
Input Value.: param contains the command line string and a pointer to globals
Return Value: 0 if everything is ok
******************************************************************************/
int input_init(input_parameter *param, int id)
{
    int i;
    if(pthread_mutex_init(&controls_mutex, NULL) != 0) {
        IPRINT("could not initialize mutex variable\n");
        exit(EXIT_FAILURE);
    }

    param->argv[0] = INPUT_PLUGIN_NAME;
    param->global->in[id].name = malloc((strlen(INPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->in[id].name, INPUT_PLUGIN_NAME);

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0 },
            {"help", no_argument, 0, 0},
            {"b", required_argument, 0, 0},
            {"bits", required_argument, 0, 0},
            {"r", required_argument, 0, 0},
            {"rate", required_argument, 0, 0},
            {"c", required_argument, 0, 0},
            {"channels", required_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* b, bits */
        case 2:
        case 3:
            DBG("case 2,3\n");
            m_bits = atoi(optarg);
            break;

            /* r, rate*/
        case 4:
        case 5:
            DBG("case 4,5\n");
            m_rate = atoi(optarg);
            break;

            /* c, channels*/
        case 6:
        case 7:
            DBG("case 4,5\n");
            m_channels = atoi(optarg);
            break;

        default:
            DBG("default case\n");
            help();
            return 1;
        }
    }

    pglobal = param->global;

    IPRINT("sampling frequency.............: %d\n", m_rate);
    IPRINT("sample bits........: %d\n", m_bits);
    IPRINT("channels........: %d\n", m_channels);
    return 0;
}

/******************************************************************************
Description.: stops the execution of the worker thread
Input Value.: -
Return Value: 0
******************************************************************************/
int input_stop(int id)
{
    DBG("will cancel input thread\n");
    pthread_cancel(worker);

    return 0;
}

/******************************************************************************
Description.: starts the worker thread and allocates memory
Input Value.: -
Return Value: 0
******************************************************************************/
int input_run(int id)
{

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }
    pthread_detach(worker);

    return 0;
}

/******************************************************************************
Description.: print help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-r | --rate ]........: delay to pause between frames\n" \
    " [-b | --bits ]....: tcp server listen port\n"\
    " [-c | --channels ]....: tcp server listen port\n"\
    " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: recv frame from output_tcp client
Input Value.: arg is not used
Return Value: NULL
******************************************************************************/
void *worker_thread(void *arg)
{

    unsigned char *databuf;
    databuf = (unsigned char*) malloc(sizeof(unsigned char)*BUFLEN);

    audio_init(m_rate,m_bits,m_channels);

    /* set cleanup handler to cleanup allocated ressources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(!pglobal->stop)
    {

        audio_read(databuf);
        printf("data[0] = %d\n",databuf[0]);
        /* copy JPG picture to global buffer */
        pthread_mutex_lock(&pglobal->in[plugin_number].db);

        pglobal->in[plugin_number].size = BUFLEN;
        pglobal->in[plugin_number].buf =  databuf;

        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);

    }
    close(fd_audio);
    databuf = NULL;

    IPRINT("leaving input thread, calling cleanup function now\n");
    pthread_cleanup_pop(1);

    return NULL;
}

/******************************************************************************
Description.: this functions cleans up allocated ressources
Input Value.: arg is unused
Return Value: -
******************************************************************************/
void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up ressources\n");
        return;
    }

    first_run = 0;
    DBG("cleaning up ressources allocated by input thread\n");

    if(pglobal->in[plugin_number].buf != NULL)
        free(pglobal->in[plugin_number].buf);
}

/******************************************************************************
Description.: process commands, allows to set v4l2 controls
Input Value.: * control specifies the selected v4l2 control's id
                see struct v4l2_queryctr in the videodev2.h
              * value is used for control that make use of a parameter.
Return Value: depends in the command, for most cases 0 means no errors and
              -1 signals an error. This is just rule of thumb, not more!
******************************************************************************/
int input_cmd(int plugin_number, unsigned int control_id, unsigned int group, int value, char *value_string)
{
    if (control_id < 3)
        pglobal->in[plugin_number].in_parameters[control_id].value = value;
    return 0;
}


