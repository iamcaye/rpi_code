/*
 ============================================================================
 Name        : PruebaPi.c
 Author      : Embedded Systems UMA
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "wiringPi/wiringPi.h"

#include "frozen.h"


#include <string.h>
#include "MQTTAsync.h"

#include "remotelink.h"
#include "remotelink_messages.h"

#define TIVA_SERIAL_PORT "/dev/ttyACM0"

#define LEFT_BUTTON              0x00000010  // GPIO pin 4
#define RIGHT_BUTTON              0x00000001  // GPIO pin 0



#define ADDRESS     "tcp://192.168.178.123:1883" /*XXX:  IP/Puerto del servidor [EDITAR] */
#define CLIENTID    "ExampleClientSub" //XXX: Editar: Cada cliente conectado a un servidor debe tener un clientID diferente.
#define SUB_TOPIC       "/rpi/controltopic" /*XXX:  Topic al que me voy a suscribir [EDITAR] */
#define PUB_TOPIC       "/rpi/datatopic" /*XXX:  Topic en el que voy a publicar [EDITAR] */
#define BTN_TOPIC	"/rpi/btntopic"
#define ADC_TOPIC	"/rpi/adctopic"
#define QOS         0
#define TIMEOUT     10000L

volatile MQTTAsync_token deliveredtoken;
MQTTAsync client; //Global para gestionar el cliente (se puede hacer más elegante, pero por simplicidad utilizamos una global)

//Globales para control de estado (de nuevo, no particularmente elegante)
static int disc_finished = 0;
static int subscribed = 0;
static int finished = 0;
static float g_frec = 0.5;

//PI_THREAD es una macro de la biblioteca WirinPi
PI_THREAD ( TestTask ); //Declaracion de un thread implementado mas abajo

//Esta callback se ejecuta cuando se pierde la conexion con el servidor MQTT
void onConnLost(void *context, char *cause)
{
	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	int rc;

	printf("\nConnection lost\n");
	printf("     cause: %s\n", cause);

	printf("Reconnecting\n");
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start connect, return code %d\n", rc);
		finished = 1;
	}
}



//Esta callback se ejecuta cuando llega un mensaje
//TopicName es una cadena con el nombre del topic, y TopicLen su longitud
//Message tiene varios campos, entre ellos "payload" (los datos) y payloadlen (su longitud)
int onMsgArrived(void *context, char *topicName, int topicLen, MQTTAsync_message *message)
{
	int i;
	bool leds[3];
	bool pins[3];
	int pins_value[3], rgb[3];
	float rgb_intensity = 0.0f;

	char* payloadptr;

	printf("Message arrived\n");
	printf("     topic: %s\n", topicName); //El topic me puede servir para hacer una cosa u otra...
	printf("   message: ");



	//Cuidado que como el formato no sea el correcto, la lio...
	payloadptr = message->payload;

	//Imprimo el mensaje por la consola
	for(i=0; i<message->payloadlen; i++)
	{
		putchar(*payloadptr++);
	}
	putchar('\n');

	//json_scanf decodifica el mensaje JSON
	//Aqui se obtienen los 3 parametros juntos, pero podria hacerse por separado, invocando 3 veces a json_scanf
	//Cada una para leer un parametro.

	if (json_scanf(message->payload, message->payloadlen, "{ redLed: %B, greenLed: %B, blueLed: %B }", &leds[0], &leds[1], &leds[2]) >= 1)
	{
		MESSAGE_LED_GPIO_PARAMETER parametro;

		parametro.leds.red=leds[0];
		parametro.leds.green=leds[1];
		parametro.leds.blue=leds[2];

		//Mando el mensaje a la TIVA
		remotelink_sendMessage(MESSAGE_LED_GPIO,&parametro,sizeof(parametro));
	}

	if (json_scanf(message->payload, message->payloadlen, "{pin2: %B, pin3: %B, pin4: %B  }", &pins[0], &pins[1], &pins[2]) >= 1)
	{
		pins_value[0] = pins[0];
		pins_value[1] = pins[1];
		pins_value[2] = pins[2];
	#if CROSS_COMPILING
		digitalWrite (2, pins_value[0]);
		digitalWrite (3, pins_value[1]);
		digitalWrite (4, pins_value[2]);
	#endif
	}

	if (json_scanf(message->payload, message->payloadlen, "{ redRGB: %d, greenRGB: %d, blueRGB: %d }", &rgb[0], &rgb[1], &rgb[2]) == 3)
	{
		remotelink_sendMessage(MESSAGE_RGB_COLOR, (void *)&rgb, sizeof(rgb));
	}

	if (json_scanf(message->payload, message->payloadlen, "{intensityRGB: %f}", &rgb_intensity) == 1)
	{
		remotelink_sendMessage(MESSAGE_LED_PWM_BRIGHTNESS, (void *)&rgb_intensity, sizeof(rgb_intensity));
	}

	if (json_scanf(message->payload, message->payloadlen, "{ADCFrec: %f}", &g_frec) == 1){
		printf("Frecuencia recibida\n");
	}

	//Libero el mensaje
	MQTTAsync_freeMessage(&message);
	MQTTAsync_free(topicName);
	return 1;
}

//Esta callback se ejecuta cuando nos desconectamos del servidor
void onDisconnect(void* context, MQTTAsync_successData* response)
{
	printf("Successful disconnection\n");
	disc_finished = 1;
}


//Esta callback se ejecuta cuando nos suscribimos correctamente al servidor
void onSubscribe(void* context, MQTTAsync_successData* response)
{
	printf("Subscribe succeeded\n");
	subscribed = 1;
}

//Esta callback se ejecuta cuando falla la suscripcion
void onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Subscribe failed, rc %d\n", response ? response->code : 0);
	finished = 1;
}

//Esta callback se ejecuta cuando falla la conexión al servidor
void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
	printf("Connect failed, rc %d\n", response ? response->code : 0);
	finished = 1;
}

//Esta callback se ejecuta cuando el envío que he realizado es confirmado...
void onSend(void* context, MQTTAsync_successData* response)
{
	printf("Message with token value %d delivery confirmed\n", response->token);

}



//Esta callback se ejecuta cuando me conecto al servidor de forma correcta...
// Se realizan dos acciones: suscribirse a un topic y lanzar el hilo que sondea el ADC.
void onConnect(void* context, MQTTAsync_successData* response)
{
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	int rc;

	printf("Successful connection\n");

	printf("Subscribing to topic %s\nfor client %s using QoS%d\n\n"
			"Press Q<Enter> to quit\n\n", SUB_TOPIC, CLIENTID, QOS);
	opts.onSuccess = onSubscribe; //Esta funcion callback se ejecutara cuando la suscripción se haya realizado correctamente
	opts.onFailure = onSubscribeFailure;
	opts.context = client;

	deliveredtoken = 0;
	//Realiza la peticion de suscripcion
	if ((rc = MQTTAsync_subscribe(client, SUB_TOPIC, QOS, &opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start subscribe, return code %d\n", rc);
		exit(-1);
	}

	piThreadCreate(TestTask); //Crea la tarea que hace el envio periodico
}



//Este thread se encarga de dos cosas enviar un mensaje a un topic de forma periódica
//Está puesta como ejemplo de envio....
PI_THREAD ( TestTask )
{

	char json_buffer[100];
	int i=0;
	int estado_led=0;

	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	//Envia el mensaje de PING
	delay(1000);
	remotelink_sendMessage(MESSAGE_PING,NULL,0);
	delay(100);

	while(1)
	{
		delay(1000*(1/g_frec));

#if CROSS_COMPILING
		//Este código sólo se compila si vamos a compilar para la RPI
		digitalWrite (19,estado_led);
#endif

		estado_led=!estado_led;

		remotelink_sendMessage(MESSAGE_ADC_SAMPLE,NULL,0);

		i=!i; //De momento publica "true" o "false" alternativamente.

		//Formatea un mensaje en JSON. El mensaje queda guardado en json_buffer.
		struct json_out out1 = JSON_OUT_BUF(json_buffer, sizeof(json_buffer));//Reinicio out1, de lo contrario se van acumulando los printfs
		json_printf(&out1,"{ boton : %B}",i);


		opts.onSuccess = onSend; //Esta funcion callback se ejecutara cuando el SEND se haya realizado correctamente
		opts.context = client;

		//Parametros del mensaje (payload y QoS)
		pubmsg.payload = json_buffer; //el payload son los datos
		pubmsg.payloadlen = strlen(json_buffer);
		pubmsg.qos = QOS;
		pubmsg.retained = 0;
		deliveredtoken = 0;

		//Envía al topic
		if ((rc = MQTTAsync_sendMessage(client, PUB_TOPIC, &pubmsg, &opts)) != MQTTASYNC_SUCCESS)
		{
			printf("Failed to start sendMessage, return code %d\n", rc);
			exit(-1);
		}

	}
}


//Funcion callback que procesa los mensajes recibidos desde el PC (ejecuta las acciones correspondientes a las ordenes recibidas)
static int32_t messageReceived(uint8_t message_type, void *parameters, int32_t parameterSize)
{
	int32_t status=0;   //Estado de la ejecucion (positivo, sin errores, negativo si error)
	char json_buffer[100];
	uint32_t botones = 0;
	bool left_btn, right_btn;
	MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
	MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
	int rc;

	//Comprueba el tipo de mensaje
	switch (message_type)
	{
	case MESSAGE_REJECTED:

	{
		MESSAGE_REJECTED_PARAMETER parametro;
		if (check_and_extract_command_param(parameters, parameterSize, &parametro, sizeof(parametro))>0)
		{
			//Imprime los datos....
			printf("Mensaje rechazado por la TIVA... %d\n",(int32_t)parametro.command);
		}
		else
		{
			status=PROT_ERROR_INCORRECT_PARAM_SIZE; //Devuelve un error
		}

	}
	break;
	case MESSAGE_PING:
	{
		printf("PING recibido desde la placa\n");
	}
	break;
	case MESSAGE_ADC_SAMPLE:
	{
		MESSAGE_ADC_SAMPLE_PARAMETER parametro;
		float muestra1;
		float muestra2;
		float muestra3;
		float muestra4;

		if (check_and_extract_command_param(parameters, parameterSize, &parametro, sizeof(parametro))>0)
		{

			muestra1=3.3*(float)parametro.chan1/4096.0;
			muestra2=3.3*(float)parametro.chan2/4096.0;
			muestra3=3.3*(float)parametro.chan3/4096.0;
			muestra4=3.3*(float)parametro.chan4/4096.0;

			//Imprime los datos por la consola de texto....
			printf("ADC... ch0: %f ch1: %f ch2: %f ch3: %f\n",muestra1,muestra2,muestra3,muestra4);

			struct json_out out1 = JSON_OUT_BUF(json_buffer, sizeof(json_buffer));//Reinicio out1, de lo contrario se van acumulando los printfs
			json_printf(&out1,"{ADCch0: %f, ADCch1: %f, ADCch2: %f, ADCch3: %f}",muestra1,muestra2,muestra3,muestra4);


			opts.onSuccess = onSend; //Esta funcion callback se ejecutara cuando el SEND se haya realizado correctamente
			opts.context = client;

			//Parametros del mensaje (payload y QoS)
			pubmsg.payload = json_buffer; //el payload son los datos
			pubmsg.payloadlen = strlen(json_buffer);
			pubmsg.qos = QOS;
			pubmsg.retained = 0;
			deliveredtoken = 0;

			//Envía al topic
			if ((rc = MQTTAsync_sendMessage(client, ADC_TOPIC, &pubmsg, &opts)) != MQTTASYNC_SUCCESS)
			{
				printf("Failed to start sendMessage, return code %d\n", rc);
				exit(-1);
			}
		}
		else
		{
			status=PROT_ERROR_INCORRECT_PARAM_SIZE; //Devuelve un error
		}
	}
	break;
	case MESSAGE_BUTTONS:
	{
		if (check_and_extract_command_param(parameters, parameterSize, &botones, sizeof(botones))>0)
		{
			if(!(botones & RIGHT_BUTTON))
			{
				right_btn = true;
			}
			else
			{
				right_btn = false;
			}

			if(!(botones & LEFT_BUTTON))
			{
				left_btn = true;
			}
			else
			{
				left_btn = false;
			}

			struct json_out out1 = JSON_OUT_BUF(json_buffer, sizeof(json_buffer));//Reinicio out1, de lo contrario se van acumulando los printfs
			json_printf(&out1,"{ rightButton : %B, leftButton: %B}", right_btn, left_btn);


			opts.onSuccess = onSend; //Esta funcion callback se ejecutara cuando el SEND se haya realizado correctamente
			opts.context = client;

			//Parametros del mensaje (payload y QoS)
			pubmsg.payload = json_buffer; //el payload son los datos
			pubmsg.payloadlen = strlen(json_buffer);
			pubmsg.qos = QOS;
			pubmsg.retained = 0;
			deliveredtoken = 0;

			//Envía al topic
			if ((rc = MQTTAsync_sendMessage(client, BTN_TOPIC, &pubmsg, &opts)) != MQTTASYNC_SUCCESS)
			{
				printf("Failed to start sendMessage, return code %d\n", rc);
				exit(-1);
			}
		}
	}
	break;
	default:
		//mensaje desconocido/no implementado
		status=PROT_ERROR_UNIMPLEMENTED_COMMAND; //Devuelve error.
	}
	return status;   //Devuelve status
}



int main(int argc, char* argv[])
{

	//Inicializacion Wiring Pi y configuracion GPIO

#if CROSS_COMPILING
	//Este código sólo se compila si vamos a compilar para la RPI
	wiringPiSetupGpio();
	pinMode (19,OUTPUT);
	pinMode (2,OUTPUT);
	pinMode (3,OUTPUT);
	pinMode (4,OUTPUT);
#endif

	//Inicializa la parte del puerto serie.... [CONEXION TIVA]
	//Lanza un thread que lee del puerto serie....
	remotelink_init(TIVA_SERIAL_PORT,messageReceived); // "/dev/ttyACM0"es el fichero de dispositivo del puerto serie que voy a abrir
	//Podia haberlo pasado como parametro de main(...) en lugar de ponerlo fijo



	MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
	MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;

	int rc;
	int ch;

	//Ahora inicializo el cliente MQTT
	MQTTAsync_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
	MQTTAsync_setCallbacks(client, NULL, onConnLost, onMsgArrived, NULL); 	//onConnLost sera la callback que se ejecuta cuando se pierde la conexion
	//onMsgArrived sera la callback que se ejecute cuando lleguen mensajes.
	conn_opts.keepAliveInterval = 20;
	conn_opts.cleansession = 1;
	conn_opts.onSuccess = onConnect; //Esta funcion callback se ejecutara cuando la conexion se haya realizado correctamente
	conn_opts.onFailure = onConnectFailure; //Esta funcion callback se ejecutara si la conexion falla
	conn_opts.context = client;

	//Conecta con el servidor MQTT
	if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start connect, return code %d\n", rc);
		exit(-1);
	}

	while	(!subscribed)
	{
		usleep(10000L);
	}

	if (finished)
		goto exit;

	do
	{
		ch = getchar();
		//Si se pulsa "Q" por la consola (stdin)...
	} while (ch!='Q' && ch != 'q');

	disc_opts.onSuccess = onDisconnect;
	if ((rc = MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS)
	{
		printf("Failed to start disconnect, return code %d\n", rc);
		exit(-1);
	}
	while	(!disc_finished)
	{
		usleep(10000L);
	}

	exit:
	MQTTAsync_destroy(&client);
	return rc;
}

