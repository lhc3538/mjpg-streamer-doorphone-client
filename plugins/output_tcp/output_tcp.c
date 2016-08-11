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

/*
  This output plugin is based on code from output_file.c
  Writen by  Hanchao Leng
  Version 0.1, Aug 2016

  Send frame to tcp server,one signal one frame.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>

#include <netdb.h>
#include <netinet/in.h>
#include <malloc.h>

#include <dirent.h>

#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "TCP output plugin"

static pthread_t worker;
static globals *pglobal;
static int max_frame_size;
static unsigned char *frame = NULL;
static int input_number = 0;

//TCP socket fd
int sock_client_tcp = -1;
// TCP server port
static int port = 3538;
//TCP server ip
static char *ip = "127.0.0.1";

/******************************************************************************
Description.: print a help message
Input Value.: -
Return Value: -
******************************************************************************/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
            " Help for output plugin..: "OUTPUT_PLUGIN_NAME"\n" \
            " ---------------------------------------------------------------\n" \
            " The following parameters can be passed to this plugin:\n\n" \
            " [-s | --ip ]........: TCP server ip address\n" \
            " [-p | --port ]..........: TCP server  port\n\n" \
            " [-i | --input ].......: read frames from the specified input plugin (first input plugin between the arguments is the 0th)\n\n" \
            " ---------------------------------------------------------------\n");
}

/******************************************************************************
Description.: clean up allocated ressources
Input Value.: unused argument
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
    OPRINT("cleaning up ressources allocated by worker thread\n");

    if(frame != NULL) {
        free(frame);
    }

    //close tcp socket
    client_tcp_destory();

}

/**
 * @brief tcp连接操作
 * @param ser_ip
 * @param ser_port
 * @return
 */
int client_tcp_init(char *ser_ip,int ser_port)
{
    struct sockaddr_in server_addr;
    struct hostent * host;
    if ((host = gethostbyname(ser_ip)) == NULL)
    {
        perror("gethostbyname 失败");
        return -1;
    }
    //客户端开始建立sockfd描述符
    if ((sock_client_tcp = socket(AF_INET,SOCK_STREAM,0)) == -1)
    {
        perror("tcp socket创建失败");
        return -1;
    }
    bzero(&server_addr,sizeof(server_addr));//初始化，置0.
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ser_port);//将本机的short数据转化为网络的short数据
    server_addr.sin_addr = *((struct in_addr *)host->h_addr);
    //客户程序发起连接请求
    if (connect(sock_client_tcp,(struct sockaddr *)(&server_addr),sizeof(struct sockaddr)) == -1)
    {
        perror("连接失败");
        return -1;
    }
    printf("tcp connect ok\n!");
    return sock_client_tcp;
}

/**
 * @brief client_tcp_sendln 发送一条数据
 * @param databuf
 * @return
 */
int client_tcp_send(unsigned char *databuf,int len)
{
    printf("len = %d ; ",len);
    if (write(sock_client_tcp,&len,sizeof(int)) == -1)
    {
        perror("tcp LEN发送失败");
        return -1;
    }
    //send databuf secondly
    if (write(sock_client_tcp,databuf,len) == -1)
    {
        perror("tcp DATA发送失败");
        return -1;
    }
    printf("data[last] = %d\n",databuf[len-1]);
    if (read(sock_client_tcp,&len,sizeof(int)) <=0 )
    {
        perror("接收ACK失败");
        return -1;
    }
    return 0;
}
/**
 * @brief client_tcp_destory 销毁tcp套接字
 * @return
 */
int client_tcp_destory()
{
    close(sock_client_tcp);
    sock_client_tcp = -1;
    printf("tcp dectory!\n");
    return 0;
}
/******************************************************************************
Description.: this is the main worker thread
              it loops forever, grabs a fresh frame and stores it to file
Input Value.:
Return Value:
******************************************************************************/
void *worker_thread(void *arg)
{
    int frame_size = 0;
    unsigned char *tmp_framebuffer = NULL;

    /* set cleanup handler to cleanup allocated ressources */
    pthread_cleanup_push(worker_cleanup, NULL);
    //连接TCP服务器
    client_tcp_init(ip,port);

    while(!pglobal->stop) {

        DBG("waiting for fresh frame\n");
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update, &pglobal->in[input_number].db);

        /* read buffer */
        frame_size = pglobal->in[input_number].size;

        /* check if buffer for frame is large enough, increase it if necessary */
        if(frame_size > max_frame_size) {
            DBG("increasing buffer size to %d\n", frame_size);

            max_frame_size = frame_size + (1 << 16);
            if((tmp_framebuffer = realloc(frame, max_frame_size)) == NULL) {
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                LOG("not enough memory\n");
                return NULL;
            }

            frame = tmp_framebuffer;
        }

        /* copy frame to our local buffer now */
        memcpy(frame, pglobal->in[input_number].buf, frame_size);

        /* allow others to access the global buffer again */
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        //send frame to tcp server
        if (client_tcp_send(frame,frame_size) < 0)
            break;

    }

    /* cleanup now */
    pthread_cleanup_pop(1);

    return NULL;
}

/*** plugin interface functions ***/
/******************************************************************************
Description.: this function is called first, in order to initialise
              this plugin and pass a parameter string
Input Value.: parameters
Return Value: 0 if everything is ok, non-zero otherwise
******************************************************************************/
int output_init(output_parameter *param)
{
    int i;

    param->argv[0] = OUTPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {"s", required_argument, 0, 0},
            {"serverip", required_argument, 0, 0},
            {"p", required_argument, 0, 0},
            {"port", required_argument, 0, 0},
            {"i", required_argument, 0, 0},
            {"input", required_argument, 0, 0},
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

            /* s, serverip */
        case 2:
        case 3:
            DBG("case 2,3\n");
                ip = malloc(strlen(optarg) + 1);
                strcpy(ip, optarg);
            break;

            /* p, port */
        case 4:
        case 5:
            DBG("case 4,5\n");
            port = atoi(optarg);
            break;
            /* i, input */
        case 6:
        case 7:
            DBG("case 6,7\n");
            input_number = atoi(optarg);
            break;
        }
    }

    pglobal = param->global;
    if(!(input_number < pglobal->incnt)) {
        OPRINT("ERROR: the %d input_plugin number is too much only %d plugins loaded\n", input_number, pglobal->incnt);
        return 1;
    }
    OPRINT("input plugin.....: %d: %s\n", input_number, pglobal->in[input_number].plugin);
    OPRINT("output server ip.....: %s\n", ip);
    OPRINT("output server port..: %d\n", port);
    return 0;
}

/******************************************************************************
Description.: calling this function stops the worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int output_stop(int id)
{
    DBG("will cancel worker thread\n");
    pthread_cancel(worker);
    return 0;
}

/******************************************************************************
Description.: calling this function creates and starts the worker thread
Input Value.: -
Return Value: always 0
******************************************************************************/
int output_run(int id)
{
    DBG("launching worker thread\n");
    pthread_create(&worker, 0, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}


