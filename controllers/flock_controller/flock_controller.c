/**************************************************************************************************
 * File:        flock_controller.c
 * Authors:     Zeki Doruk Erden, Michael Perret, Mickaël Salamin
 * Date:        Fall 2018
 * Description: Controller for DIS project.
 *************************************************************************************************/

/*** Specific libraries ***/

#include <webots/robot.h>
#include <webots/emitter.h>
#include <webots/receiver.h>
#include <webots/motor.h>
#include <webots/position_sensor.h>
#include <webots/distance_sensor.h>

/*** Standard libraries ***/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <time.h>

/*** Symbolic constants ***/

#define TIME_STEP  64 // Step duration [ms]
#define FLOCK_SIZE 5  // Number of robots in flock

#define NB_SENSORS 8    // Number of distance sensors
#define MIN_SENS   350  // Minimum sensibility value
#define MAX_SENS   4096 // Maximum sensibility value

#define MAX_SPEED       800     // Maximum speed for differential drive [mm/s]
#define MAX_SPEED_MOTOR 6.28    // Maximum speed for rotational motor [rad/s]
#define AXLE_LENGTH     0.052   // Distance between wheels of robot [m]
#define SPEED_UNIT_RADS 0.00628 // Conversion factor from speed unit to radian per second
#define WHEEL_RADIUS    0.0205  // Wheel radius [m]
#define DELTA_T         0.064   // Timestep [s]

#define NEIGHBORHOOD_THRESHOLD 0.40      // Maximum neighborhood radius [m]
#define RULE1_THRESHOLD        0.20      // Threshold to activate aggregation rule (default 0.20)
#define RULE2_THRESHOLD        0.15      // Threshold to activate dispersion rule (default 0.15)
#define RULE1_WEIGHT           (0.6/10)  // Weight of aggregation rule (default 0.6/10)
#define RULE2_WEIGHT           (0.02/10) // Weight of dispersion rule (default 0.02/10)
#define RULE3_WEIGHT           (1.0/10)  // Weight of consistency rule (default 1.0/10)
#define MIGRATION_WEIGHT       (0.01/10) // Weight of attraction towards the common goal (default 0.01/10)
#define MIGRATORY_URGE         1         // If the robots should just go forward or move towards a specific migratory direction

/*** Symbolic macros ***/

#define ABS(x) ((x >= 0) ? (x) : -(x))

/*** Data types definition ***/

// Structure representing a two-dimensional vector
struct Vector2D {
	float x; // x-coordinate
	float y; // y-coordinate
};
typedef struct Vector2D Vec2D;

// Structure representing a three-dimensional vector
struct Vector3D {
	float x; // x value
	float z; // z value
	float t; // theta value
};
typedef struct Vector3D Vec3D;

/*** Global constants ***/

static const int e_puck_matrix[16] = {17,29,34,10,8,-38,-56,-76,-72,-58,-36,8,10,36,28,18}; // Weights for obstacle avoidance

/*** Global variables ***/

static char* robot_name;      // Robot's unique identification name
static unsigned int robot_id; // Robot's unique identification number

static WbDeviceTag left_motor, right_motor;     // Rotational motors
static WbDeviceTag left_encoder, right_encoder; // Position sensors
static WbDeviceTag emitter, receiver;           // Radio sensors
static WbDeviceTag sensors[NB_SENSORS];         // Distance sensors

static float      my_position[3];              // X, Z, Theta of the current robot
static float prev_my_position[3];              // X, Z, Theta of the current robot in the previous time step
static float      relative_pos[FLOCK_SIZE][3]; // Relative X, Z, Theta of all robots
static float prev_relative_pos[FLOCK_SIZE][3]; // Relative X, Z, Theta of all robots in the previous time step
static float          speed[FLOCK_SIZE][2];    // Speed of all robots
static float relative_speed[FLOCK_SIZE][2];    // Speed of all robots relative to the current robot

static float migr[2] = {25, -25}; // Migration vector

/*** Functions declaration ***/

unsigned long mix(unsigned long a, unsigned long b, unsigned long c);
void init(void);
void limit(int *number, const int limit);
void compute_wheel_speeds(int *msl, int *msr);
void update_self_motion(const int msl, const int msr);
void send_ping(void);
void process_received_ping_messages(void);
void reynolds_rules(void);

/*** Functions implementation ***/

/*
 * Robert Jenkins' 96 bit Mix Function
 * http://web.archive.org/web/20070111091013/http://www.concentric.net/~Ttwang/tech/inthash.htm
 * http://web.archive.org/web/20070110173527/http://burtleburtle.net:80/bob/hash/doobs.html
 */
unsigned long mix(unsigned long a, unsigned long b, unsigned long c)
{
    a=a-b;  a=a-c;  a=a^(c >> 13);
    b=b-c;  b=b-a;  b=b^(a << 8);
    c=c-a;  c=c-b;  c=c^(b >> 13);
    a=a-b;  a=a-c;  a=a^(c >> 12);
    b=b-c;  b=b-a;  b=b^(a << 16);
    c=c-a;  c=c-b;  c=c^(b >> 5);
    a=a-b;  a=a-c;  a=a^(c >> 3);
    b=b-c;  b=b-a;  b=b^(a << 10);
    c=c-a;  c=c-b;  c=c^(b >> 15);
    return c;
}

/*
 * Initialize the controller.
 */
void init(void)
{
	// Seed the random number generator
	const unsigned long seed = mix(clock(), time(NULL), getpid());
	srand(seed);
	
	// Unique identifier of the robot
	robot_name = (char*) wb_robot_get_name();
	sscanf(robot_name, "epuck%d", &robot_id);
	
	// Radio emitter and receiver
	emitter = wb_robot_get_device("emitter");
	receiver = wb_robot_get_device("receiver");
	wb_receiver_enable(receiver, TIME_STEP);
	
	// Wheel motors and encoders
    left_motor = wb_robot_get_device("left wheel motor");
	right_motor = wb_robot_get_device("right wheel motor");
	left_encoder = wb_robot_get_device("left wheel sensor");
	right_encoder = wb_robot_get_device("right wheel sensor");
    wb_position_sensor_enable( left_encoder, TIME_STEP);
    wb_position_sensor_enable(right_encoder, TIME_STEP);
	wb_motor_set_position( left_motor, INFINITY);
	wb_motor_set_position(right_motor, INFINITY);
	wb_motor_set_velocity( left_motor, 0);
	wb_motor_set_velocity(right_motor, 0);
	
	// Distance sensors
	char sensor_name[] = "ps0";
	for (unsigned int i = 0; i < NB_SENSORS; i++) {
		sensors[i] = wb_robot_get_device(sensor_name);
		wb_distance_sensor_enable(sensors[i], TIME_STEP);
		sensor_name[2]++;
	}
}

/*
 * Keep given integer within interval [-limit, +limit].
 */
void limit(int *number, const int limit)
{
	if (*number > limit)
		*number = limit;
	if (*number < -limit)
		*number = -limit;
}

/*
 * Compute wheel speeds from a given (X,Z) speed.
 */
void compute_wheel_speeds(int *msl, int *msr)
{
	// Compute wanted position from Reynold's speed and current location
	float x =  speed[robot_id][0]*cosf(my_position[2]) + speed[robot_id][1]*sinf(my_position[2]); // x in robot coordinates
	float z = -speed[robot_id][0]*sinf(my_position[2]) + speed[robot_id][1]*cosf(my_position[2]); // z in robot coordinates
	//printf("id = %d, x = %f, y = %f\n", robot_id, x, z);
	
	// Proportional controller on tangential and angular speeds
	float Ku = 0.2; // Forward control coefficient
	float Kw = 1; // Rotational control coefficient
	float range = sqrtf(x*x + z*z); // Distance to the wanted position
	float bearing = -atan2(x, z); // Orientation of the wanted position
	
	// Compute forward control
	float u = Ku*range*cosf(bearing);
	// Compute rotational control
	float w = Kw*bearing;
	
	// Convert to wheel speeds
	*msl = (u - AXLE_LENGTH*w/2.0) * (1000.0/WHEEL_RADIUS);
	*msr = (u + AXLE_LENGTH*w/2.0) * (1000.0/WHEEL_RADIUS);
	limit(msl, MAX_SPEED);
	limit(msr, MAX_SPEED);
	//printf("bearing = %f, u = %f, w = %f, msl = %f, msr = %f\n", bearing, u, w, msl, msr);
}

/*
 * Update robot's position with wheel speeds.
 */
void update_self_motion(const int msl, const int msr)
{
	const float theta = my_position[2];
	
	// Compute deltas of the robot
	const float dr = (float)msr * SPEED_UNIT_RADS * WHEEL_RADIUS * DELTA_T;
	const float dl = (float)msl * SPEED_UNIT_RADS * WHEEL_RADIUS * DELTA_T;
	const float du = (dr + dl)/2.0;
	const float dtheta = (dr - dl)/AXLE_LENGTH;
	
	// Compute deltas in the environment
	const float dx = -du * sinf(theta);
	const float dz = -du * cosf(theta);
	
	// Update position
	my_position[0] += dx;
	my_position[1] += dz;
	my_position[2] += dtheta;
	
	// Keep orientation within [0, 2pi]
	if (my_position[2] > 2*M_PI) my_position[2] -= 2.0*M_PI;
	if (my_position[2] < 0)      my_position[2] += 2.0*M_PI;
}

/*
 * Each robot sends a ping message, so that the other robots can measure the relative range and bearing to the sender.
 * The message contains the robot's name.
 * The range and bearing will be measured directly out of message RSSI and direction.
 */
void send_ping(void)
{
	char out[10];
	strcpy(out, robot_name); // In the ping message we send the name of the robot
	wb_emitter_send(emitter, out, strlen(out)+1); 
}

/*
 * Process all the received ping messages, and calculate range and bearing to the other robots.
 * The message contains the robot's name.
 * The range and bearing are measured directly out of message RSSI and direction.
 */
void process_received_ping_messages(void)
{
	const double *message_direction; // Direction of the emitter with respect to the receiver's coordinate system
	double message_rssi; // Received Signal Strength Indicator
	double theta;
	double range;
	char *inbuffer;	// Buffer for the receiver node
	int other_robot_id;
	
	while (wb_receiver_get_queue_length(receiver) > 0) {
		inbuffer = (char*) wb_receiver_get_data(receiver);
		message_direction = wb_receiver_get_emitter_direction(receiver);
		message_rssi = wb_receiver_get_signal_strength(receiver);
		double x = message_direction[1];
		double y = message_direction[2];
		
		theta =	-atan2(y, x);
		theta = theta + my_position[2]; // Relative theta
		range = sqrt((1/message_rssi));
		
		// Teammate's ID is retreived from the robot's name contained in the message
		sscanf(inbuffer, "epuck%d", &other_robot_id);
		
		/* Position update */
		prev_relative_pos[other_robot_id][0] = relative_pos[other_robot_id][0];
		prev_relative_pos[other_robot_id][1] = relative_pos[other_robot_id][1];
		
		relative_pos[other_robot_id][0] =  range*cos(theta); // Relative x-coordinate
		relative_pos[other_robot_id][1] = -range*sin(theta); // Relative y-coordinate
		
		//printf("Robot %s, from robot %d, x=%g, y=%g, theta=%g, my_theta=%g\n",robot_name,other_robot_id,relative_pos[other_robot_id][0],relative_pos[other_robot_id][1],-atan2(y,x)*180.0/3.141592,my_position[2]*180.0/3.141592);
		
		relative_speed[other_robot_id][0] = relative_speed[other_robot_id][0]*0.0 + 1.0*(1/DELTA_T)*(relative_pos[other_robot_id][0]-prev_relative_pos[other_robot_id][0]);
		relative_speed[other_robot_id][1] = relative_speed[other_robot_id][1]*0.0 + 1.0*(1/DELTA_T)*(relative_pos[other_robot_id][1]-prev_relative_pos[other_robot_id][1]);
		
		wb_receiver_next_packet(receiver);
	}
}

/*
 * Update speed according to Reynold's rules.
 */
void reynolds_rules(void)
{
	unsigned int i, j;  			// Loop counters
	unsigned int n_robots = 1;      // Number of neighbors
	float rel_avg_loc[2] = {0,0};   // Flock average relative positions
	float rel_avg_speed[2] = {0,0}; // Flock average relative speeds
	float cohesion[2] = {0,0};      // Aggregation behavior
	float dispersion[2] = {0,0};    // Separation behavior
	float consistency[2] = {0,0};   // Alignment behavior
	
	/* Compute averages over the flockmates in the local neighborhood */
	for (i = 0; i < FLOCK_SIZE; i++) {
		if (i == robot_id)
			continue; // Don't consider yourself for the average
		// If robot i is in the local neighborhood (Euclidean distance)
		if (sqrt(pow(relative_pos[i][0],2)+pow(relative_pos[i][1],2)) < NEIGHBORHOOD_THRESHOLD) {
			for (j = 0; j < 2; j++) {
				rel_avg_loc[j] += relative_pos[i][j];
				rel_avg_speed[j] += relative_speed[i][j];
			}
			n_robots++;
		}
	}
	for (j = 0; j < 2; j++) {
		if (n_robots > 1) {
			rel_avg_loc[j] /= n_robots-1;
			rel_avg_speed[j] /= n_robots-1;
		} else {
			rel_avg_loc[j] = speed[robot_id][j];
			rel_avg_speed[j] = my_position[j];
		}
	}
	
	/* Rule 1 - Aggregation/Cohesion: move towards the center of mass */
	// If center of mass is too far (Euclidean distance)
	if (sqrt(pow(rel_avg_loc[0],2)+pow(rel_avg_loc[1],2)) > RULE1_THRESHOLD) {
		for (j = 0; j < 2; j++) {
			cohesion[j] = rel_avg_loc[j]; // Average relative position
		}
	}
	
	/* Rule 2 - Dispersion/Separation: keep far enough from flockmates */
	for (i = 0; i < FLOCK_SIZE; i++) {
		if (i != robot_id) { // Loop on flockmates only
			// If neighbor i is too close (Euclidean distance)
			if (pow(relative_pos[i][0],2)+pow(relative_pos[i][1],2) < RULE2_THRESHOLD) {
				for (j = 0; j < 2; j++) {
					dispersion[j] -= 1/relative_pos[i][j]; // Relative distance to neighbor i
				}
			}
		}
	}
	
	/* Rule 3 - Consistency/Alignment: match the speeds of flockmates */
	for (j = 0; j < 2; j++) {
		consistency[j] = rel_avg_speed[j]; // Average relative speed
	}
	
	// Aggregation of all behaviors with relative influence determined by weights
	for (j = 0; j < 2; j++) {
		speed[robot_id][j]  = cohesion   [j] * RULE1_WEIGHT;
		speed[robot_id][j] += dispersion [j] * RULE2_WEIGHT;
		speed[robot_id][j] += consistency[j] * RULE3_WEIGHT;
	}
	speed[robot_id][1] *= -1; // y-axis of Webots is inverted
	
	// Move the robot according to some migration rule
	if (MIGRATORY_URGE == 0) {
		speed[robot_id][0] += 0.01*cos(my_position[2] + M_PI/2);
		speed[robot_id][1] += 0.01*sin(my_position[2] + M_PI/2);
	} else {
		speed[robot_id][0] += (migr[0]-my_position[0]) * MIGRATION_WEIGHT;
		speed[robot_id][1] -= (migr[1]-my_position[1]) * MIGRATION_WEIGHT; // y-axis of Webots is inverted
	}
}

/*
 * Main function.
 */
int main(int argc, char *args[])
{
	// Variables declaration
	unsigned int i;            // Loop counter
	int msl = 0, msr = 0;      // Wheel speeds for differential drive
	float msl_w, msr_w;        // Wheel speeds for rotational motors
	int bmsl, bmsr;            // Braitenberg parameters
	int distances[NB_SENSORS]; // Array for the distance sensor readings
	int sum_sensors;           // Sum of all distance sensor readings
	int max_sens;              // Store highest sensor value
	
	// Initialization
	wb_robot_init();
	init();
	
	// Simulation loop
	while (wb_robot_step(TIME_STEP) != -1) {
		bmsl = 0; bmsr = 0;
		sum_sensors = 0;
		max_sens = 0;
		
		/* Braitenberg */
		for (i = 0; i < NB_SENSORS; i++) {
			distances[i] = wb_distance_sensor_get_value(sensors[i]); // Read sensor values
			sum_sensors += distances[i]; // Add up sensor values
			max_sens = distances[i] > max_sens ? distances[i] : max_sens; // Check if new highest sensor value
			
			// Weighted sum of distance sensor values for Braitenberg vehicle
			bmsr += e_puck_matrix[i]            * distances[i];
			bmsl += e_puck_matrix[i+NB_SENSORS] * distances[i];
		}
		
		// Adapt Braitenberg values (empirical tests)
		bmsl /= MIN_SENS; bmsr /= MIN_SENS;
		bmsl += 66; bmsr += 72;
		
		/* Send and get information */
		send_ping(); // Sending a ping to other robots, so they can measure their distance to this robot
		
		// Compute self position
		prev_my_position[0] = my_position[0];
		prev_my_position[1] = my_position[1];
		
		update_self_motion(msl, msr);
		
		process_received_ping_messages();
		
		speed[robot_id][0] = (1/DELTA_T)*(my_position[0]-prev_my_position[0]);
		speed[robot_id][1] = (1/DELTA_T)*(my_position[1]-prev_my_position[1]);
		
		// Reynold's rules with all previous info (updates the speed[][] table)
		reynolds_rules();
		
		// Compute wheels speed from Reynold's speed
		compute_wheel_speeds(&msl, &msr);
		
		// Adapt speed instinct to distance sensor values
		if (sum_sensors > NB_SENSORS*MIN_SENS) {
			msl -= msl*max_sens/(2*MAX_SENS);
			msr -= msr*max_sens/(2*MAX_SENS);
		}
		
		// Add Braitenberg
		msl += bmsl;
		msr += bmsr;
		limit(&msl, MAX_SPEED);
		limit(&msr, MAX_SPEED);
		
		// Set speed
		msl_w = msl*MAX_SPEED_MOTOR/1000;
		msr_w = msr*MAX_SPEED_MOTOR/1000;
		wb_motor_set_velocity( left_motor, msl_w);
		wb_motor_set_velocity(right_motor, msr_w);
	}
	
    // Clean up and exit
    wb_robot_cleanup();
    return 0;
}