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

#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/wait.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "TCP input plugin"

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;
static pthread_mutex_t controls_mutex;
static int plugin_number;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

static int delay = 100;
static int port = 3538;

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
            {"d", required_argument, 0, 0},
            {"delay", required_argument, 0, 0},
            {"p", required_argument, 0, 0},
            {"port", required_argument, 0, 0},
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

            /* d, delay */
        case 2:
        case 3:
            DBG("case 2,3\n");
            delay = atoi(optarg);
            break;

            /* p, port */
        case 4:
        case 5:
            DBG("case 4,5\n");
            port = atoi(optarg);
            break;

        default:
            DBG("default case\n");
            help();
            return 1;
        }
    }

    pglobal = param->global;

    IPRINT("delay.............: %i\n", delay);
    IPRINT("listen port........: %d\n", port);
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
    " [-d | --delay ]........: delay to pause between frames\n" \
    " [-p | --port]....: tcp server listen port\n"
    " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: recv frame from output_tcp client
Input Value.: arg is not used
Return Value: NULL
******************************************************************************/
void *worker_thread(void *arg)
{
    int sockfd,new_fd;
    struct sockaddr_in my_addr; /* 本机地址信息 */
    struct sockaddr_in their_addr; /* 客户地址信息 */
    unsigned int sin_size, lisnum;

    unsigned char *databuf;
    databuf = (unsigned char*)malloc(sizeof(char));
    //接收数据长度
    int len;
    //实际接受长度
    int accept_len;
    int rul;

    lisnum = 2;

    if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    printf("socket %d ok \n",port);

    my_addr.sin_family=PF_INET;
    my_addr.sin_port=htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(my_addr.sin_zero), 0);
    if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1) {
        perror("bind");
        exit(1);
    }
    printf("bind ok \n");

    if (listen(sockfd, lisnum) == -1) {
        perror("listen");
        exit(1);
    }
    printf("listen ok \n");

    sin_size = sizeof(struct sockaddr_in);

    /* set cleanup handler to cleanup allocated ressources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(!pglobal->stop)
    {
        if ((new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
            perror("accept");
            exit(0);
        }
        printf("server: got connection from %s\n",inet_ntoa(their_addr.sin_addr));

        rul = 1;
        while (rul > 0)
        {
            //接收长度数据
            accept_len = 0;
            while(1)
            {
                rul = read(new_fd,&len+accept_len,sizeof(int)-accept_len) ;
                if(rul <= 0)
                {
                    perror("tcp接收长度数据失败");
                    break;
                }
                accept_len += rul;
                if (accept_len == sizeof(int))
                    break;
            }
            printf("len = %d ; ",len);
            //按照接收到的长度分配内存
            databuf = (unsigned char*)realloc(databuf,sizeof(char)*len);
            if (databuf == NULL)
            {
                perror("malloc failed");
                break;
            }
            //接收数据
            accept_len = 0;
            while(1)
            {
                rul = read(new_fd,databuf+accept_len,len-accept_len) ;
                if(rul<= 0)
                {
                    perror("tcp接收数据失败");
                    break;
                }
                accept_len += rul;
                if (accept_len == len)
                    break;
            }
            printf("data[last] = %d\n",databuf[len-1]);
            /* copy JPG picture to global buffer */
            pthread_mutex_lock(&pglobal->in[plugin_number].db);

            pglobal->in[plugin_number].size = len;
            pglobal->in[plugin_number].buf =  databuf;

            /* signal fresh_frame */
            pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
            pthread_mutex_unlock(&pglobal->in[plugin_number].db);

            //发送ACK
            if (write(new_fd,&len,sizeof(int)) == -1)
            {
                perror("发送ACK失败");
                break;
            }
        }
        close(new_fd);
    }

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


