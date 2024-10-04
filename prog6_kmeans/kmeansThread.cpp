#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>

#include "CycleTimer.h"

using namespace std;

typedef struct {
  // Control work assignments
  int start, end;

  // Multithreading arguments
  int startM, endM;
  double *minDist;

  // Shared by all functions
  double *data;
  double *clusterCentroids;
  int *clusterAssignments;
  double *currCost;
  int M, N, K;
} WorkerArgs;

// double distTime = 0, assignmentsTime = 0, centroidsTime = 0, costTime = 0;
static const int numThreads = 8;

/**
 * Checks if the algorithm has converged.
 * 
 * @param prevCost Pointer to the K dimensional array containing cluster costs 
 *    from the previous iteration.
 * @param currCost Pointer to the K dimensional array containing cluster costs 
 *    from the current iteration.
 * @param epsilon Predefined hyperparameter which is used to determine when
 *    the algorithm has converged.
 * @param K The number of clusters.
 * 
 * NOTE: DO NOT MODIFY THIS FUNCTION!!!
 */
static bool stoppingConditionMet(double *prevCost, double *currCost,
                                 double epsilon, int K) {
  for (int k = 0; k < K; k++) {
    if (abs(prevCost[k] - currCost[k]) > epsilon)
      return false;
  }
  return true;
}

/**
 * Computes L2 distance between two points of dimension nDim.
 * 
 * @param x Pointer to the beginning of the array representing the first
 *     data point.
 * @param y Poitner to the beginning of the array representing the second
 *     data point.
 * @param nDim The dimensionality (number of elements) in each data point
 *     (must be the same for x and y).
 */
double dist(double *x, double *y, int nDim) {
  // double start = CycleTimer::currentSeconds();
  double accum = 0.0;
  for (int i = 0; i < nDim; i++) {
    accum += pow((x[i] - y[i]), 2);
  }
  // double end = CycleTimer::currentSeconds();
  // distTime += end - start;
  return sqrt(accum);
}

/**
 * Assigns each data point to its "closest" cluster centroid.
 */
void computeAssignments(WorkerArgs *const args) {
  // double *minDist = new double[args->M];
  // 
  // Initialize arrays
  for (int m = args->startM; m < args->endM; m++) {
    args->minDist[m] = 1e30;
    // args->clusterAssignments[m] = -1;
  }

  // Assign datapoints to closest centroids
  for (int m = args->startM; m < args->endM; m++) {
    for (int k = args->start; k < args->end; k++) {
      double d = dist(&args->data[m * args->N],
                      &args->clusterCentroids[k * args->N], args->N);
      if (d < args->minDist[m]) {
        args->minDist[m] = d;
        args->clusterAssignments[m] = k;
      }
    }
  }

  // free(minDist);
}

/**
 * Given the cluster assignments, computes the new centroid locations for
 * each cluster.
 */
void computeCentroids(WorkerArgs *const args) {
  int *counts = new int[args->K];

  // Zero things out
  for (int k = 0; k < args->K; k++) {
    counts[k] = 0;
    for (int n = 0; n < args->N; n++) {
      args->clusterCentroids[k * args->N + n] = 0.0;
    }
  }


  // Sum up contributions from assigned examples
  for (int m = 0; m < args->M; m++) {
    int k = args->clusterAssignments[m];
    for (int n = 0; n < args->N; n++) {
      args->clusterCentroids[k * args->N + n] +=
          args->data[m * args->N + n];
    }
    counts[k]++;
  }

  // Compute means
  for (int k = 0; k < args->K; k++) {
    counts[k] = max(counts[k], 1); // prevent divide by 0
    for (int n = 0; n < args->N; n++) {
      args->clusterCentroids[k * args->N + n] /= counts[k];
    }
  }

  free(counts);
}

/**
 * Computes the per-cluster cost. Used to check if the algorithm has converged.
 */
void computeCost(WorkerArgs *const args) {
  double *accum = new double[args->K];

  // Zero things out
  for (int k = 0; k < args->K; k++) {
    accum[k] = 0.0;
  }

  // Sum cost for all data points assigned to centroid
  for (int m = 0; m < args->M; m++) {
    int k = args->clusterAssignments[m];
    accum[k] += dist(&args->data[m * args->N],
                     &args->clusterCentroids[k * args->N], args->N);
  }

  // Update costs
  for (int k = args->start; k < args->end; k++) {
    args->currCost[k] = accum[k];
  }

  free(accum);
}

/**
 * Computes the K-Means algorithm, using std::thread to parallelize the work.
 *
 * @param data Pointer to an array of length M*N representing the M different N 
 *     dimensional data points clustered. The data is layed out in a "data point
 *     major" format, so that data[i*N] is the start of the i'th data point in 
 *     the array. The N values of the i'th datapoint are the N values in the 
 *     range data[i*N] to data[(i+1) * N].
 * @param clusterCentroids Pointer to an array of length K*N representing the K 
 *     different N dimensional cluster centroids. The data is laid out in
 *     the same way as explained above for data.
 * @param clusterAssignments Pointer to an array of length M representing the
 *     cluster assignments of each data point, where clusterAssignments[i] = j
 *     indicates that data point i is closest to cluster centroid j.
 * @param M The number of data points to cluster.
 * @param N The dimensionality of the data points.
 * @param K The number of cluster centroids.
 * @param epsilon The algorithm is said to have converged when
 *     |currCost[i] - prevCost[i]| < epsilon for all i where i = 0, 1, ..., K-1
 */
void kMeansThread(double *data, double *clusterCentroids, int *clusterAssignments,
               int M, int N, int K, double epsilon) {
  static constexpr int MAX_THREADS = 32;

  // Used to track convergence
  double *prevCost = new double[K];
  double *currCost = new double[K];

  // The WorkerArgs array is used to pass inputs to and return output from
  // functions.
  WorkerArgs args[MAX_THREADS];

  // Create worker array
  std::thread workers[MAX_THREADS];

  // Initialize arrays to track cost
  for (int k = 0; k < K; k++) {
    prevCost[k] = 1e30;
    currCost[k] = 0.0;
  }

  /* Main K-Means Algorithm Loop */
  int iter = 0;
  double *minDist = new double[M];
  while (!stoppingConditionMet(prevCost, currCost, epsilon, K)) {
    // Update cost arrays (for checking convergence criteria)
    for (int k = 0; k < K; k++) {
      prevCost[k] = currCost[k];
    }

    // Setup args struct
    for (int i = 0; i < numThreads; i++) {
      args[i].start = 0;
      args[i].end = K;

      args[i].data = data;
      args[i].clusterCentroids = clusterCentroids;
      args[i].clusterAssignments = clusterAssignments;
      args[i].currCost = currCost;
      args[i].M = M;
      args[i].N = N;
      args[i].K = K;

      args[i].startM = i * M/numThreads;
      args[i].endM = (i+1) * M/numThreads;
      args[i].minDist = minDist;

      if (i == numThreads-1)
        args[i].endM = M;
    }

    for (int i = 1; i < numThreads; i++) {
      workers[i] = std::thread(computeAssignments, &args[i]);
    }

    // double s = CycleTimer::currentSeconds();
    computeAssignments(&args[0]);
    for (int i = 1; i < numThreads; i++) {
      workers[i].join();
    }
    // double t = CycleTimer::currentSeconds();
    computeCentroids(&args[0]);
    // double u = CycleTimer::currentSeconds();
    computeCost(&args[0]);
    // double v = CycleTimer::currentSeconds();


    // assignmentsTime += t - s;
    // centroidsTime += u - t;
    // costTime += v - u;

    iter++;

  }

  // printf("Total time spent in dist is %.3f ms.\n", distTime * 1000);
  // printf("Total time spent in computeAssignments is %.3f ms.\n", assignmentsTime * 1000);
  // printf("Total time spent in computeCentroids is %.3f ms.\n", centroidsTime * 1000);
  // printf("Total time spent in computeCost is %.3f ms.\n", costTime * 1000);

  free(minDist);
  free(currCost);
  free(prevCost);
}
