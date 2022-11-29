#include <unistd.h>  
#include <stdlib.h>  
#include <stdio.h>  
#include <iostream>

int main( int argc, char *argv[], char *env[])  
{  
    char * args[] = {(char*) "/home/nvidia/workspace/wqg/QingLong/hmi",(char*) NULL };  
  	std::cout << "argc = " << argc << std::endl;
    std::cout << "-------------------BEGIN OUTPUT ARGV-----------------" << std::endl;
    int i = 0;
    while(argv[i])
    {
	    std::cout << i << ">>" << argv[i] << std::endl;
	    i++;
    }
    std::cout << "--------------END ARGV------ BEGIN OUT PUT ENV----------------" << std::endl;
	  int j = 0;
    while(env[j])
    {
	    std::cout << env[j++] << std::endl;
    }
    std::cout << "===========END ENV===========" << std::endl;
    if(argc > 1)
    {
      pid_t childPid = 0;
      childPid = fork();
      if(childPid == 0)
	    {
        std::cout << "argv[1] = " << argv[1] << std::endl;
        const char *workdir = "/home/nvidia/workspace/wqg/QingLong/";
        chdir(workdir);
        int ret = execve(argv[1], &argv[1], env); 
        if (  -1 == ret )  
        { 
          std::cout << "failed to execve" << std::endl; 
            perror( "execve" );  
            exit( EXIT_FAILURE);  
        }
        else
        {
          std::cout << "success to execve " << workdir << argv[1] << std::endl;
        }
      }
      else
      {
        std::cout << "main create childPid = " << childPid << std::endl;
      }
    }
    puts( "shouldn't get here" );  
    exit( EXIT_SUCCESS );  
} 
