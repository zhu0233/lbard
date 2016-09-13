#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#ifdef linux
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>

#include "sync.h"
#include "lbard.h"
#include "serial.h"


struct experiment_data {
  int packet_number;
  int packet_len;
  int gap_us;
  int pulse_width_us;
  int pulse_frequency;
  int wifiup_hold_time_us;
  int key;

  // Used on the slave side only, but included in the packets for convenience
  double cycle_duration;
  double duty_cycle;
  int speed;
};

int pulse_widths[]={43,86,173,260,520,1041,2083,4166,8333,33333,0};





// From os.c in serval-dna
long long gettime_us()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1)
    return -1;
  if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    return -1;
  return nowtv.tv_sec * 1000000LL + nowtv.tv_usec;
}

char *wifi_interface_name=NULL;
int wifi_fd=-1;

static int wifi_disable()
{
#ifdef linux
  fprintf(stderr,"Disabling wifi interface %s @ %lldms\n",
	  wifi_interface_name,gettime_ms());
  if (wifi_fd==-1)
    wifi_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, wifi_interface_name);
  if (ioctl(wifi_fd,SIOCGIFFLAGS,&ifr)) {
    perror("SIOCGIFFLAGS failed");
    return -1;
  }
  ifr.ifr_flags&=!IFF_UP;
  if (ioctl(wifi_fd,SIOCSIFFLAGS,&ifr)) {
    perror("SIOCSIFFLAGS failed");
    return -1;
  }
#else
  fprintf(stderr,"wifi_disable() not implemented for this platform.\n");
  return -1;
#endif
  return 0;
}

static int wifi_enable()
{
#ifdef linux
  fprintf(stderr,"Enabling wifi interface %s @ %lldms\n",
	  wifi_interface_name,gettime_ms());
 if (wifi_fd==-1)
   wifi_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
 struct ifreq ifr;
 memset(&ifr, 0, sizeof(ifr));
 strcpy(ifr.ifr_name, wifi_interface_name);
 if (ioctl(wifi_fd,SIOCGIFFLAGS,&ifr)) {
   perror("SIOCGIFFLAGS failed");
   return -1;
 }
 ifr.ifr_flags|=IFF_UP;
 if (ioctl(wifi_fd,SIOCSIFFLAGS,&ifr)) {
    perror("SIOCSIFFLAGS failed");
   return -1;
 }
#else
  fprintf(stderr,"wifi_disable() not implemented for this platform.\n");
  return -1;
#endif 
  return 0;
}


int setup_experiment(struct experiment_data *exp)
{
  exp->cycle_duration = 1000.0/exp->pulse_frequency;
  exp->duty_cycle=exp->pulse_width_us/10.0/exp->cycle_duration;
  
  fprintf(stderr,"Running energy sample experiment:\n");
  fprintf(stderr,"  pulse width = %.4fms\n",exp->pulse_width_us/1000.0);
  fprintf(stderr,"  pulse frequency = %dHz\n",exp->pulse_frequency);
  fprintf(stderr,"  cycle duration (1/freq) = %3.2fms",exp->cycle_duration);
  fprintf(stderr,"  duty cycle = %3.2f%%\n",exp->duty_cycle);
  fprintf(stderr,"  wifi hold time = %.1fms\n",exp->wifiup_hold_time_us/1000.0);

  if (exp->duty_cycle>99) {
    fprintf(stderr,"ERROR: Duty cycle cannot exceed 99%%\n");
    exit(-1);
  }

  if (exp->duty_cycle>90) {
    fprintf(stderr,"WARNING: Duty cycle is close to 100%% -- accuracy may suffer.\n");
  }
    
  // Work out correct serial port speed to produce the required pulse width
  int speed=-1;
  int possible_speeds[]={230400,115200,57600,38400,19200,9600,4800,2400,1200,300,0};
  int s;
  for(s=0;possible_speeds[s];s++) {
    // Pulse width will be 10 serial ticks wide for the complete character.
    int this_pulse_width=1000000*10/possible_speeds[s];
    if (((this_pulse_width-exp->pulse_width_us)<10)&&
	((this_pulse_width-exp->pulse_width_us)>-10))
      {
	speed=possible_speeds[s];
	break;
      }
  }
  if (speed==-1) {
    fprintf(stderr,
	    "Could not find a speed setting for pulse width of %.4fms (%dusec).\n",
	    exp->pulse_width_us/1000.0,exp->pulse_width_us);
    fprintf(stderr,"Possible pulse widths are:\n");
    for(s=0;possible_speeds[s];s++) {
      int this_pulse_width=1000000*10/possible_speeds[s];
      fprintf(stderr,"  %.4fms (%dusec)\n",
	      this_pulse_width/1000.0,
	      this_pulse_width);
    }
    return -1;
  }
  exp->speed=speed;
  return 0;
}

int energy_experiment(char *port, int pulse_frequency,float pulse_width_ms,
		      int wifiup_hold_time_ms, char *interface_name)
{
  wifi_interface_name=interface_name;

  struct experiment_data exp;

  exp.pulse_width_us=pulse_width_ms*1000.0;
  exp.pulse_frequency=pulse_frequency;
  exp.wifiup_hold_time_us=wifiup_hold_time_ms*1000;
  int experiment_valid=setup_experiment(&exp);
  
  int serialfd=-1;
  serialfd = open(port,O_RDWR);
  if (serialfd<0) {
    perror("Opening serial port");
    exit(-1);
  }
    fprintf(stderr,"Serial port open as fd %d\n",serialfd);

  while (1) {
    if (experiment_valid)
      if (serial_setup_port_with_speed(serialfd,exp.speed))
	{
	  fprintf(stderr,"Failed to setup serial port. Exiting.\n");
	  exit(-1);
	}

    int pulse_interval_usec=1000000.0/exp.pulse_frequency;
    fprintf(stderr,"Sending a pulse every %dusec to achieve %dHz\n",
	    pulse_interval_usec,pulse_frequency);

    // Start with wifi down
    int wifi_down=1;
    wifi_disable();
    long long wifi_down_time=0;
    
    int missed_pulses=0,sent_pulses=0;
    long long next_time=gettime_us();
    long long report_time=gettime_us()+1000;
    char nul[1]={0};
    while(1) {
      long long now=gettime_us();
      if (now>report_time) {
	report_time+=1000000;
	if ((sent_pulses != pulse_frequency)||missed_pulses)
	  fprintf(stderr,"Sent %d pulses in the past second, and missed %d deadlines (target is %d).\n",
		  sent_pulses,missed_pulses,pulse_frequency);
	sent_pulses=0;
	missed_pulses=0;
      }
      if (now>=next_time) {
	// Next pulse is due, so write a single character of 0x00 to the serial port so
	// that the TX line is held low for 10 serial ticks (or should the byte be 0xff?)
	// which will cause the energy sampler to be powered for that period of time.
	write(serialfd, nul, 1);
	sent_pulses++;
	// Work out next time to send a character to turn on the energy sampler.
	// Don't worry about pulses that we can't send because we lost time somewhere,
	// just keep track of how many so that we can report this to the user.
	next_time+=pulse_interval_usec;
	while(next_time<now) {
	  next_time+=pulse_interval_usec;
	  missed_pulses++;
	}
      } else {
	// Wait for a little while if we have a while before the next time we need
	// to send a character. But busy wait the last 10usec, so that it doesn't matter
	// if we get woken up fractionally late.
	// Watcharachai will need to use an oscilliscope to see how adequate this is.
	// If there is too much jitter, then we will need to get more sophisticated.
	if (next_time-now>10) usleep(next_time-now-10);
      }
      char buf[1024];
      ssize_t bytes = read_nonblock(serialfd, buf, sizeof buf);
      if (bytes>0) {
	// Work out when to take wifi low
	wifi_down_time=gettime_us()+wifiup_hold_time_ms*1000;
	fprintf(stderr,"Saw energy on channel @ %lldms, holding Wi-Fi for %lld more usec\n",
		gettime_ms(),wifi_down_time-gettime_us());
	if (wifi_down) { wifi_enable(); wifi_down=0; }
      } else {
	if (now>wifi_down_time) {
	  if (wifi_down==0) wifi_disable();
	  wifi_down=1;
	}
      }
    }    

  }
  return 0;
}

int packet_number=0;
int build_packet(unsigned char *packet,
		 int gap_us,int packet_len,int pulse_width_us,
		 int pulse_frequency,int wifiup_hold_time_us,
		 int key)
{

  // Make packet empty
  bzero(packet,packet_len);

  // Copy packet information
  struct experiment_data p;
  p.packet_number=++packet_number;
  p.packet_len=packet_len;
  p.gap_us=gap_us;
  p.pulse_width_us=pulse_width_us;
  p.pulse_frequency=pulse_frequency;
  p.wifiup_hold_time_us=wifiup_hold_time_us;
  p.key=key;
  bcopy(&p,packet,sizeof(p));
  
  return 0;
}

int send_packet(int sock,unsigned char *packet,int len)
{
  struct sockaddr_in addr;
  bzero(&addr, sizeof(addr)); 
  addr.sin_family = AF_INET; 
  addr.sin_port = htons(19002);

  sendto(sock,packet,len,
	 MSG_DONTROUTE|MSG_DONTWAIT
#ifdef MSG_NOSIGNAL
	 |MSG_NOSIGNAL
#endif	       
	 ,(const struct sockaddr *)&addr,sizeof(addr));
  return 0;
}

int run_energy_experiment(int sock,
			  int gap_us,int packet_len,int pulse_width_us,
			  int pulse_frequency,int wifiup_hold_time_us)
{
  // First, send a chain of packets until we get a reply acknowledging that
  // the required mode has been selected.
  unsigned char packet[1500];

  int key=random();
  
  build_packet(packet,gap_us,packet_len,pulse_width_us,
	       pulse_frequency,wifiup_hold_time_us,
	       key);
  send_packet(sock,packet,packet_len);
  printf("Sent packet with key 0x%08x\n",key);

  // Then wait 3 seconds to ensure that we everything is flushed through
  time_t timeout=time(0)+3;
  while(time(0)<timeout) {
    unsigned char rx[9000];
    int r=recvfrom(sock,rx,9000,0,NULL,0);
    if (r>0) {
      struct experiment_data *pd=(void *)&rx[0];
      printf("Saw packet with key 0x%08x\n",pd->key);
    }
  }


  // Then run experiment:
  // Send 100 packet pairs, with enough delay between them to ensure wifi has
  // switched off again, and keep track of the number of replies we receive, and
  // then log the result of the experiment.


  return 0;
}


int energy_experiment_master()
{
  int sock=socket(AF_INET, SOCK_DGRAM, 0);
  if (sock==-1) {
    perror("Could not create UDP socket");
    exit(-1);
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(19001);
  bind(sock, (struct sockaddr *) &addr, sizeof(addr));
  set_nonblock(sock);
  
  // Enable broadcast
  int one=1;
  int r=setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  if (r) {
    fprintf(stderr,"WARNING: setsockopt(): Could not enable SO_BROADCAST\n");
  }

  /*
    For this experiment we vary the gap between sending pairs of packets,
    and keep track of the percentage of replies in each case.  The client side
    will be using the energy sampler program to control its wifi interface.
    We will also send it the parameters it should be using.
  */
  int gap_us;
  int packet_len;
  int pulse_width_us;
  int pulse_frequency;
  int wifiup_hold_time_us;
  for(gap_us=500;gap_us<10000000;gap_us*=1.5) {
    for(packet_len=100;packet_len<=1500;packet_len+=100) {
      for(pulse_width_us=500;pulse_width_us<100000;pulse_width_us*=1.5) {
	for(pulse_frequency=1;(pulse_frequency*pulse_width_us)<=900000;
	    pulse_frequency+=5) {
	  for(wifiup_hold_time_us=1000;wifiup_hold_time_us<5000000;
	      wifiup_hold_time_us*=1.5) {
	    run_energy_experiment(sock,
				  gap_us,packet_len,pulse_width_us,
				  pulse_frequency,wifiup_hold_time_us);
	  }
	}
      }
    }
  }
  return 0;
}
