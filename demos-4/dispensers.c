
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
int shmid,dispensadores,numSets,i,letra,piezas[4],total;
key_t key;
key = 2222;

signal(SIGINT, ctrl_c_handler);
if (argc != 8) {
	fprintf(stderr,"usage: <#dispensadores,#sets, #piezasA, #piezasB, #piezasC, #piezasD, usec-interval >\n");	
	exit(-1);
}
if (atoi(argv[1]) <= 0||atoi(argv[2]) <= 0||atoi(argv[3]) <= 0||atoi(argv[3]) <= 0||atoi(argv[4]) <= 0||atoi(argv[5]) <= 0||atoi(argv[6]) <= 0||atoi(argv[7]) <= 0) {
	fprintf(stderr,"Argumentos deben ser non-negativos\n");	
	exit(-1);
}
dispensadores=atoi(argv[1]);	
printf("Total de dispensadores %d\n",dispensadores);
numSets=atoi(argv[2]);
piezas[0]=numSets*atoi(argv[3]);
piezas[1]=numSets*atoi(argv[4]);
piezas[2]=numSets*atoi(argv[5]);
piezas[3]=numSets*atoi(argv[6]);

printf("Piezas por set %d\n",atoi(argv[3])+atoi(argv[4])+atoi(argv[5])+atoi(argv[6]));
printf("Numero de sets %d\n",numSets);
total=piezas[0]+piezas[1]+piezas[2]+piezas[3];
printf("Total de piezas %d\n",total);

// uso segmento de memoria compartida para compartir datos
if ((shmid = shmget(key, dispensadores, IPC_CREAT | 0666)) < 0) {
	perror("shmget");
 	exit(-1);
}
if ((shm = (int *)shmat(shmid, NULL, 0)) == (int *) -1) {
	perror("shmat");
   	exit(-1);
}

for (i=0;i<dispensadores;i++){ //pongo en no pieza el dispensador
  *(shm+i)=-0;
}
sleep(2); //simulo preparacion del despacho de piezas

while (total>0){
    usleep((unsigned int)atoi(argv[7])); //intervalo entre eventos de dispensado
    for (i=0;i<dispensadores;i++){
      letra=(int)rand()%5; 
      *(shm+i)=0; //no pieza
      if (letra<4)  
         if (piezas[letra]>0) {
           piezas[letra]--;
           *(shm+i)=letra+1;//tipo de pieza 1-4
         }
      printf("%d ",*(shm+i));   
    }

    printf("\n");    
    total=piezas[0]+piezas[1]+piezas[2]+piezas[3];  
}
for (i=0;i<dispensadores;i++){
  *(shm+i)=-1;
}

if (shmdt(shm) == -1) {
	perror("shmat");
   	exit(-1);
}
  
return(0);

}
