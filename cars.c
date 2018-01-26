#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "traffic.h"
#include "errno.h"

extern struct intersection isection;

/**
 * Populate the car lists by parsing a file where each line has
 * the following structure:
 *
 * <id> <in_direction> <out_direction>
 *
 * Each car is added to the list that corresponds with 
 * its in_direction
 * 
 * Note: this also updates 'inc' on each of the lanes
 */
void parse_schedule(char *file_name) {
    int id;
    struct car *cur_car;
    struct lane *cur_lane;
    enum direction in_dir, out_dir;
    FILE *f = fopen(file_name, "r");

    /* parse file */
    while (fscanf(f, "%d %d %d", &id, (int*)&in_dir, (int*)&out_dir) == 3) {

        /* construct car */
        cur_car = malloc(sizeof(struct car));
        cur_car->id = id;
        cur_car->in_dir = in_dir;
        cur_car->out_dir = out_dir;

        /* append new car to head of corresponding list */
        cur_lane = &isection.lanes[in_dir];
        cur_car->next = cur_lane->in_cars;
        cur_lane->in_cars = cur_car;
        cur_lane->inc++;
    }

    fclose(f);
}

/**
 * TODO: Fill in this function
 *
 * Do all of the work required to prepare the intersection
 * before any cars start coming
 * 
 */
void init_intersection() {
	
	int i;
	
	//initializing CVs for each lane. 
	for (i = 0; i < 4; i++) {
	   	pthread_cond_init(&(isection.lanes[i].producer_cv), NULL);
		pthread_cond_init(&(isection.lanes[i].consumer_cv), NULL);
		pthread_mutex_init(&(isection.lanes[i].lock), NULL);
		pthread_mutex_init(&(isection.quad[i]), NULL);
		isection.lanes[i].in_cars = NULL;//malloc(sizeof(struct car));
		isection.lanes[i].out_cars = NULL;//malloc(sizeof(struct car));
		isection.lanes[i].buffer = malloc(sizeof(struct car*)*LANE_LENGTH);
		isection.lanes[i].inc = 0;
		isection.lanes[i].passed = 0;
		isection.lanes[i].head = 0;
		isection.lanes[i].tail = 0;
		isection.lanes[i].in_buf = 0;
		isection.lanes[i].capacity = LANE_LENGTH;
	}
}


/**
 * TODO: Fill in this function
 *
 * Populates the corresponding lane with cars as room becomes
 * available. Ensure to notify the cross thread as new cars are
 * added to the lane.
 * 
 */
//producer
void *car_arrive(void *arg) {

	//arg is a lane. it is a correct lane :)
    struct lane *l = arg;

	
	int moreCars = 1;

	while(moreCars == 1){
	
	//lock the lane
    pthread_mutex_lock(&(l->lock));


	if(l->in_buf + l->passed == l->inc){
		pthread_mutex_unlock(&(l->lock));
		return NULL; //done, stop
	}

    while(l->in_buf == l->capacity){
	    // wait for space in the lane 
		pthread_cond_wait(&(l->producer_cv),&(l->lock));
    }

    struct car *nextCar = l->in_cars;

   	//head == tail when the buffer is empty. don't increment tail
   	if(l->buffer[l->tail] == NULL){
		l->buffer[l->tail] = nextCar;	
	}
	else{
	
		l->tail = (l->tail+1)%LANE_LENGTH;///maintains the cycle, if the lastcar was 10, and the new car is now 11, the index will become 1, not 11
		//we are the producer, we only edit the tail, let the consumer touch the head.
		l->buffer[l->tail] = nextCar;
	}

	l->in_buf++;
	
	//check if there are more cars coming
	if(l->passed + l->in_buf != l->inc){
		moreCars = 1;
	}
	else{
		moreCars = 0;
	}

	//increment the in_cars list
	l->in_cars = l->in_cars->next;


	pthread_cond_signal(&(l->consumer_cv));
	pthread_mutex_unlock(&(l->lock));

	}

    return NULL;
}

/**
 * TODO: Fill in this function
 *
 * Moves cars from a single lane across the intersection. Cars
 * crossing the intersection must abide the rules of the road
 * and cross along the correct path. Ensure to notify the
 * arrival thread as room becomes available in the lane.
 *
 * Note: After crossing the intersection the car should be added
 * to the out_cars list of the lane that corresponds to the car's
 * out_dir. Do not free the cars!
 *
 * 
 * Note: For testing purposes, each car which gets to cross the 
 * intersection should print the following three numbers on a 
 * new line, separated by spaces:
 *  - the car's 'in' direction, 'out' direction, and id.
 * 
 * You may add other print statements, but in the end, please 
 * make sure to clear any prints other than the one specified above, 
 * before submitting your final code. 
 */

//consumer

void *car_cross(void *arg) {
    
	struct lane *l = arg;

	int moreCars = 1;

    while(moreCars == 1){
	
	//lock the lane	
   	pthread_mutex_lock(&(l->lock));	

	if(l->passed == l->inc){
		pthread_mutex_unlock(&(l->lock));
		return NULL; //done, stop
	}




    while(l->in_buf == 0){
		pthread_cond_wait(&(l->consumer_cv),&(l->lock));
    }
	
	struct car *theCar; 
	
	theCar = l->buffer[l->head];
	if(!theCar){
		pthread_mutex_unlock(&(l->lock));
		return NULL;
	}

	int* path = compute_path(theCar->in_dir,theCar->out_dir);//get in and out direction of car
	if(path[0] == -1){
		pthread_mutex_unlock(&(l->lock));

		return NULL;
	}

	int i;
	int intersectionBusy = 0;
   	for(i = 0; i < 4; i++){

		if(path[i] != 0){

			if(pthread_mutex_trylock(&(isection.quad[path[i]-1])) == EBUSY){
				int j;
				for(j = 0; j < i; j++){
					pthread_mutex_unlock(&(isection.quad[path[j]-1]));
				}
				intersectionBusy = 1;
				break;
			}
		}
	}
	// one of the quads were already locked,
	if(intersectionBusy==1){
		free(path);
		pthread_mutex_unlock(&(l->lock));
		continue;
	}

	l->head = (l->head+1)%LANE_LENGTH; //we just removed the head car above, increment head value by 1
	


	struct car *outCar = isection.lanes[theCar->out_dir].out_cars;//  first car for now

	// If outcar is none set outCar to theCar, else loop to the last car in the list.
	if(outCar == NULL){
		theCar->next = NULL;
		isection.lanes[theCar->out_dir].out_cars = theCar;
	}
	else{

		while(outCar->next != NULL){
			//*		printf("loop?\n");
				outCar = outCar->next; // ok we found the end of the outCar list
		}
		theCar->next = NULL;
		outCar->next = theCar;
	}

	//update values of in_buf and passed
	l->in_buf--;//we added an outCar, the amount of cars passed increased by 1, amount of cars in_buf decreased by 1.	
	l->passed++;
	printf("%d %d %d\n",theCar->in_dir, theCar->out_dir, theCar->id);
	int k;
  	for(k = 0; k < 4; k++){
		if(path[k] != 0){
			pthread_mutex_unlock(&(isection.quad[path[k]-1]));
		}
	}

	if(l->passed != l->inc){
		moreCars=1;
	}
	else{
		moreCars = 0;
	}

	free(path);

    pthread_cond_signal(&(l->producer_cv));

    pthread_mutex_unlock(&(l->lock));

    }
    
    return NULL;


}

/**
 * TODO: Fill in this function
 *
 * Given a car's in_dir and out_dir return a sorted 
 * list of the quadrants the car will pass through.
 * 
 */
int *compute_path(enum direction in_dir, enum direction out_dir) {

	int *path = malloc(sizeof(int)*4);

	if (in_dir == NORTH) {

		if (out_dir == NORTH) {
			path[0] = 1;
			path[1] = 2;
			path[2] = 3;
			path[3] = 4;
		}
		else if (out_dir == EAST) {
			path[0] = 2;
			path[1] = 3;
			path[2] = 4;
			path[3] = 0;
		}
		else if(out_dir == SOUTH) {
			path[0] = 2;
			path[1] = 3;
			path[2] = 0;
			path[3] = 0;
		}
		else if(out_dir == WEST){
			path[0] = 2;
			path[1] = 0;
			path[2] = 0;
			path[3] = 0;
		}

	}
	else if (in_dir == EAST) {

		if (out_dir == NORTH) {
			path[0] = 1;
			path[1] = 0;
			path[2] = 0;
			path[3] = 0;
		}
		else if (out_dir == EAST) {
			path[0] = 1;
			path[1] = 2;
			path[2] = 3;
			path[3] = 4;
		}
		else if(out_dir == SOUTH) {
			path[0] = 1;
			path[1] = 2;
			path[2] = 3;
			path[3] = 0;

		}
		else if(out_dir == WEST){
			path[0] = 1;
			path[1] = 2;
			path[2] = 0;
			path[3] = 0;

		}

	}
	else if(in_dir == SOUTH) {

		if (out_dir == NORTH) {
			path[0] = 1;
			path[1] = 4;
			path[2] = 0;
			path[3] = 0;
		}
		else if (out_dir == EAST) {
			path[0] = 4;
			path[1] = 0;
			path[2] = 0;
			path[3] = 0;
		}
		else if(out_dir == SOUTH) {
			path[0] = 1;
			path[1] = 2;
			path[2] = 3;
			path[3] = 4;
		}
		else if(out_dir == WEST){
			path[0] = 1;
			path[1] = 2;
			path[2] = 4;
			path[3] = 0;
		}

	}
	else if(in_dir == WEST){

		if (out_dir == NORTH) {
			path[0] = 1;
			path[1] = 3;
			path[2] = 4;
			path[3] = 0;
		}
		else if (out_dir == EAST) {
			path[0] = 3;
			path[1] = 4;
			path[2] = 0;
			path[3] = 0;
		}
		else if(out_dir == SOUTH) {
			path[0] = 3;
			path[1] = 0;
			path[2] = 0;
			path[3] = 0;
		}	
		else if(out_dir == WEST){
			path[0] = 1;
			path[1] = 2;
			path[2] = 3;
			path[3] = 4;
		}

	}
	else{
		path[0] = -1;
	}

    return path;
}
