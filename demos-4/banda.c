
#include <unistd.h>
#include <math.h> 
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int *shm;
    
void ctrl_c_handler(int sig_num) {
    char c;
    signal(SIGINT, SIG_IGN); 
    printf("\nDispensadores desconectados\n");
    if (shmdt(shm) == -1) {
	perror("shmat");
   	exit(-1);
    }
    exit(0); // Exit 
}


int main(int argc, char *argv[])
{
    int shmid,dispensadores,i;
    key_t key;
    key = 2222;
    
    signal(SIGINT, ctrl_c_handler);
    if (argc != 3) {
		fprintf(stderr,"usage: < #dispensadores usec-interval >\n");	
		exit(-1);
    }
    if (atoi(argv[1]) <= 0||atoi(argv[2]) <= 0){
		fprintf(stderr,"Argumentos deben ser non-negativos\n");	
		exit(-1);
	}
	
    dispensadores=atoi(argv[1]);		
    if ((shmid = shmget(key, dispensadores, 0666)) < 0) {
        perror("shmget");
        return(1);
    }
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        return(1);
    }
    while (*shm!=-1){
    	usleep((unsigned int)atoi(argv[2])); //intervalo entre eventos de dispensado
		for (i=0;i<dispensadores;i++){
  			printf("%d",*(shm+i));
		}
		printf("\n");    
    }
    
    
    return(0);

}
