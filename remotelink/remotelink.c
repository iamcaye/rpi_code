/*
 * remotelink.c
 *
 * Implementa la funcionalidad RPC entre la TIVA y el interfaz de usuario
 */

#include<stdbool.h>
#include<stdint.h>
#include <unistd.h>
#include<wiringPi.h>
#include<wiringSerial.h>
#include<stdio.h>

#include "remotelink.h"

static uint8_t Rxframe[MAX_FRAME_SIZE];	//Usar una global permite ahorrar pila en la tarea de RX.
static uint8_t Txframe[MAX_FRAME_SIZE]; //Usar una global permite ahorrar pila en las tareas, pero hay que tener cuidado al transmitir desde varias tareas!!!!
static uint32_t gRemoteProtocolErrors=0;
static int g_fidSerial;



/* Envia una trama usando la funcion bloqueante */
static int32_t remotelink_send_frame(int fd, uint8_t *frame, int32_t FrameSize)
{
	int32_t cuenta=0;
	int32_t status;

    while(cuenta<FrameSize)
    {
    	status=write (fd, (frame+cuenta), FrameSize-cuenta);
    	if (status<0)
    		return status;
    	else
    		cuenta+=status;
    }

	return cuenta;
}


// TODO
//Ojo!! TxFrame es global (para ahorrar memoria de pila en las tareas) --> Se deben tomar precauciones al usar esta funcion varias tareas
//IDEM en lo que respecta al envio por la conexion USB serie desde varias tareas....
//Estas precauciones no se han tomado en este codigo de parti1da, pero al realizar la practica se deberian tener en cuenta....
// "TODO" (por hacer)
int32_t remotelink_sendMessage(uint8_t message_type,void *parameter,int32_t paramsize)
{
    int32_t numdatos;

    numdatos=create_frame(Txframe,message_type,parameter,paramsize,MAX_FRAME_SIZE);
	if (numdatos>=0)
	{
		remotelink_send_frame(g_fidSerial,Txframe,numdatos);
	}


    return numdatos;
}

//Funciones internas

//Puntero a funcion callback que va a recibir los mensajes. Esta funcion se instala al llamar a remotelink_init
static int32_t (* remotelink_messageReceived)(uint8_t message_type, void *parameters, int32_t parameterSize);

/* Recibe una trama (sin los caracteres de inicio y fin */
/* Utiliza la funcion bloqueante xSerialGetChar ---> Esta funcion es bloqueante y no se puede llamar desde una ISR !!!*/
// Esta funcion es INTERNA de la biblioteca y solo se utiliza en la rarea TivaRPC_ServerTask
static int32_t remotelink_waitForMessage(uint8_t *frame, int32_t maxFrameSize)
{
	int32_t i;
	uint8_t rxByte;
	int status;

	do
	{
		//Elimino bytes de la cola de recepcion hasta llegar a comienzo de nueva trama
		do{
			status=serialGetchar(g_fidSerial);
			//Serialgetchar no realiza un control de errores, si quisieramos hacer control de errores deberiamas
		} while (status<0);
		rxByte=status;

	} while (rxByte!=START_FRAME_CHAR);

	i=0;
	do
	{
		//Elimino bytes de la cola de recepcion hasta llegar a comienzo de nueva trama
		do{
			status=serialGetchar(g_fidSerial);
			//Serialgetchar no realiza un control de errores, si quisieramos hacer control de errores deberiamas
		} while (status<0);
		*frame=(uint8_t)(status&0x0FF);
		i++;
	} while ((*(frame++)!=STOP_FRAME_CHAR)&&(i<=maxFrameSize));

	if (i>maxFrameSize)
	{
		return PROT_ERROR_RX_FRAME_TOO_LONG;	//Numero Negativo indicando error
	}
	else
	{
		return (i-END_SIZE);	//Devuelve el numero de bytes recibidos (quitando el de BYTE DE STOP)
	}
}

// Codigo para procesar los comandos recibidos a traves del canal USB del micro ("conector lateral")
//Esta tarea decodifica los comandos y ejecuta la función que los procesa
//También gestiona posibles errores en la comunicacion
static PI_THREAD( remotelink_Task ){

    int32_t numdatos;
    uint8_t command;
    void *ptrtoreceivedparam;
    int32_t error_status=0;

    for(;;)
    {
        //Espera hasta que se reciba una trama con datos serializados por el interfaz USB
        numdatos=remotelink_waitForMessage(Rxframe,MAX_FRAME_SIZE); //Esta funcion es bloqueante

        if (numdatos>0)
        {
            //Si no ha habido errores recibiendo la trama, la intenta decodificar y procesar
            //primero se "desestufa" y se comprueba el checksum
            numdatos=destuff_and_check_checksum(Rxframe,numdatos);
            if (numdatos<0)
            {
                //Error de checksum (PROT_ERROR_BAD_CHECKSUM), ignorar el paquete
                gRemoteProtocolErrors++;
                // Procesamiento del error (TODO, POR HACER!!)
            }
            else
            {
                //El paquete esta bien, luego procedo a tratarlo.
                //Obtiene el valor del campo comando
                command=decode_command_type(Rxframe);
                //Obtiene un puntero al campo de parametros y su tamanio.
                numdatos=get_command_param_pointer(Rxframe,numdatos,&ptrtoreceivedparam);

                //Llamo a la funcion que procesa la orden que nos ha llegado desde el PC
                error_status=remotelink_messageReceived(command,ptrtoreceivedparam,numdatos);

                //Una vez ejecutado, se comprueba si ha habido errores.
                switch(error_status)
                {
                    //Se procesarían a continuación
                    case PROT_ERROR_NOMEM:
                    {
                        // Procesamiento del error NO MEMORY
                        printf("Remotelink Error: not enough memory\n");
                    }
                    break;
                    case PROT_ERROR_STUFFED_FRAME_TOO_LONG:
                    {
                        // Procesamiento del error STUFFED_FRAME_TOO_LONG
                        printf("Remotelink Error: Frame too long. Cannot be created\n");
                    }
                    break;
                    case PROT_ERROR_COMMAND_TOO_LONG:
                    {
                        // Procesamiento del error COMMAND TOO LONG
                        printf("Remotelink Error: Packet too long. Cannot be allocated\n");
                    }
                    break;
                    case PROT_ERROR_INCORRECT_PARAM_SIZE:
                    {
                        // Procesamiento del error INCORRECT PARAM SIZE
                        printf("Remotelink Error: Incorrect parameter size\n");
                    }
                    break;
                    case PROT_ERROR_UNIMPLEMENTED_COMMAND:
                    {
//                        MESSAGE_REJECTED_PARAMETER parametro;
//                        parametro.command=command;
//                        numdatos=remotelink_sendMessage(MESSAGE_REJECTED,&parametro,sizeof(parametro));
                        printf("Remotelink Error: Unexpected command: %x\n",(uint32_t)command);
                        gRemoteProtocolErrors++;
                        //Aqui se podria, ademas, comprobar numdatos....
                    }
                    break;
                        //AÑadir casos de error adicionales aqui...
                    default:
                        /* No hacer nada */
                    break;
                }
            }
        }
        else
        { // if (numdatos >0)
            //Error de recepcion de trama(PROT_ERROR_RX_FRAME_TOO_LONG), ignorar el paquete
            gRemoteProtocolErrors++;
            // Procesamiento del error (TODO)
        }
    }
}


//Inicializa el thread que recibe comandos e instala la callback para procesarlos.
int32_t remotelink_init(char *puerto, int32_t (* message_receive_callback)(uint8_t message_type, void *parameters, int32_t parameterSize))
{



    //Abre el puerto
    g_fidSerial=serialOpen (puerto, 115200);
    if (g_fidSerial<0)
    {
    	printf("Remote: Error abriendo puerto serie\n");
    	return g_fidSerial;
    }


    //Instala la callback...
    remotelink_messageReceived=message_receive_callback;

    //
   // Crea el thread tarea que gestiona los mensajes
   //
    int error;
    error=piThreadCreate(remotelink_Task);
    if (error!=0)
    {
    	printf("Remote: Error creando el thread de recepción \n");
    	return error;
    }

    return 0;
}





