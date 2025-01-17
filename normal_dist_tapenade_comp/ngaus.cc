#include <iostream>
#include <chrono>
#include "string.h"
#include "clad/Differentiator/Differentiator.h"
#include <adBuffer.h>

//#define N 100

int parseLine(char* line){
  // This assumes that a digit will be found and the line ends in " Kb".
  int i = strlen(line);
  const char* p = line;
  while (*p <'0' || *p > '9') p++;
  line[i-3] = '\0';
  i = atoi(p);
  return i;
}

int getValue(){ //Note: this value is in KB!
  FILE* file = fopen("/proc/self/status", "r");
  int result = -1;
  char line[128];

  while (fgets(line, 128, file) != NULL){
    if (strncmp(line, "VmSize:", 7) == 0){
      result = parseLine(line);
      break;
    }
  }
  fclose(file);
  return result;
}

double gauss(double* x, double* p, double sigma, int dim) {
  double t = 0;
  for (int i = 0; i< dim; i++)
    t += (x[i] - p[i]) * (x[i] - p[i]);
  t = -t / (2*sigma*sigma);
  return std::pow(2*M_PI, -dim/2.0) * std::pow(sigma, -0.5) * std::exp(t);
}

/*
  Differentiation of gauss in reverse (adjoint) mode:
   gradient     of useful results: *retval
   with respect to varying inputs: *retval *p *x sigma
   RW status of diff variables: retval:(loc) *retval:in-out p:(loc)
   *p:out x:(loc) *x:out sigma:out
   Plus diff mem management of: retval:in p:in x:in
*/
void gauss_d_tap(double *x, double *xb, double *p, double *pb, double sigma, 
		 double *sigmab, int dim, double *retval, double *retvalb) {
  double t = 0;
  double tb = 0.0;
  double temp;
  double tempb;
  //double M_PI = 3.1415;
  for (int i = 0; i < dim; ++i)
    t = t + (x[i]-p[i])*(x[i]-p[i]);
  pushReal8(t);
  t = -t/(2*sigma*sigma);
  tempb = pow((M_PI*2), (-(dim/2.0)))*retvalb[0];
  retvalb[0] = 0.0;
  *sigmab = -(0.5*pow(sigma, -1.5)*exp(t)*tempb);
  tb = exp(t)*pow(sigma, -0.5)*tempb;
  popReal8(&t);
  temp = 2*(sigma*sigma);
  *sigmab = *sigmab + 2*2*sigma*t*tb/(temp*temp);
  tb = -(tb/temp);
  {
    double tempb;
    *pb = 0.0;
    *xb = 0.0;
    for (int i = dim-1; i > -1; --i) {
      tempb = 2*(x[i]-p[i])*tb;
      xb[i] = xb[i] + tempb;
      pb[i] = pb[i] - tempb;
    }
  }
}



typedef void(*func) (double* x, double* p, double sigma, int dim, 
                     clad::array_ref<double> _d_x, clad::array_ref<double> _d_p,
                     clad::array_ref<double> _d_sigma
		     //   clad::array_ref<double> _d_dim
		     );

//Body to be generated by Clad
void gauss_grad_0_1_2(double* x, double* p, double sigma, int dim, 
		      clad::array_ref<double> _d_x,
		      clad::array_ref<double> _d_p,
		      clad::array_ref<double> _d_sigma
		//		clad::array_ref<double> _d_dim
		      );

auto gauss_g = clad::gradient(gauss, "x, p, sigma");

//Device function pointer
func p_gauss = gauss_grad_0_1_2;

void compute(func op, double* d_x, double* d_y,
	     double* d_sigma, int i, int dim, double* result_dx,
	     double* result_dy) {

  double result_dim[2] = {}; //we don't track the derivatives of sigma and dim
  double retval;
  (*op)(&d_x[i*dim],&d_y[i*dim], d_sigma[i], dim, &result_dx[i*dim], &result_dy[i*dim],
	&result_dim[0]);//, &result_dim[1]); -- remove dim from the deriviative..
}

void compute_tap(double* d_x, double* d_y,
		 double* d_sigma, int i, int dim, double* result_dx,
		 double* result_dy) {

  double result_dim[2] = {}; //we don't track the derivatives of sigma and dim
  double retval=0.; 
  double retvalb=0.;
  gauss_d_tap(&d_x[i*dim], &result_dx[i*dim],
	      &d_y[i*dim], &result_dy[i*dim],
	      d_sigma[i], &result_dim[0],
	      dim,
	      &retval,&retvalb);
}
//void gauss_d_tap(double *x, double *xb, double *p, double *pb, double sigma, 
//		 double *sigmab, int dim, double *retval, double *retvalb) {



int main(int argc, char *argv[]) {

  // x and y point to the host arrays, allocated with malloc in the typical
  //fashion, and the d_x and d_y arrays point to device arrays allocated with
  // the cudaMalloc function from the CUDA runtime API

  unsigned int N=100;
  if (argc>1) N=atoi(argv[1]);
  unsigned int dim=1;
  if (argc>2) dim=atoi(argv[2]);

  unsigned int alloc = N*dim;  
  if (alloc / N != dim) {
    std::cout << "overflowing inputs" << std::endl;
    return 1;
  }

  double *x;
  double *y;
  double *sigma;
  x = (double*)malloc(N*dim*sizeof(double));
  y = (double*)malloc(N*dim*sizeof(double));
  sigma = (double*)malloc(N*sizeof(double)); //sigmas are shared

  // The host code will initialize the host arrays

  for (int i = 0; i < N*dim; i++) {
    x[i] = rand()%100;
    y[i] = rand()%100;
  }
  for (int i = 0; i < N; i++) {
    sigma[i] = rand()%100;
  }

  auto start = std::chrono::high_resolution_clock::now();

   // Similar to the x,y arrays, we employ host and device results array so
   //that we can copy the computed values from the device back to the host
  double *result_x, *result_y;
  result_x = (double*)malloc(N*dim*sizeof(double));
  result_y = (double*)malloc(N*dim*sizeof(double));

  auto stop = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);

  start = std::chrono::high_resolution_clock::now();


   // The computation kernel is launched by the statement:
  for ( unsigned int i=0; i<N; i++)
    compute(p_gauss, x, y, sigma, i, dim, result_x, result_y);

  stop = std::chrono::high_resolution_clock::now();

  auto duration_clad = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);

  double res_check[100];
  double res_checky[100];
  for ( unsigned int i=0; i<100; i++) 
    if ( i< N*dim ) {
      res_check[i]=result_x[i];
      res_checky[i]=result_y[i];
    }

  start = std::chrono::high_resolution_clock::now();
   // The computation kernel is launched by the statement:
  for ( unsigned int i=0; i<N; i++)
    compute_tap( x, y, sigma, i, dim, result_x, result_y);

  stop = std::chrono::high_resolution_clock::now();

  auto duration_tap = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);

  for ( unsigned int i=0; i<100; i++) 
    if ( i< N*dim )
      if ( (res_check[i]!=result_x[i]) || (res_checky[i]!=result_y[i]) ) {
	std::cout <<"Result mismatch " << i << " " << res_check[i] << " " << result_x[i];
	std::cout << " " << res_checky[i] << " " << result_y[i] << std::endl;
      }

  std::cout << "Memory alloc time " << duration.count() << " microsec" << std::endl;
  std::cout << "Clad time " <<duration_clad.count() << " microsec " << (duration_clad.count()/float(N)) << " per call to compute" <<std::endl;
  std::cout << "Tapenade time " << duration_tap.count() << " microsec " << (duration_tap.count()/float(N)) << " per call to compute" <<std::endl;
  std::cout << "Allocations made " << int((N*dim*sizeof(double)*4 + N*sizeof(double))/1.e6) << " MB "<< std::endl;
  std::cout << "process " << int(getValue()/1000.) << " MB " <<std::endl;
  //  std::cout << "Time per N (microsec) " << dim << " " << (duration.count()/float(N)) << std::endl; 

  //for ( unsigned int i=0; i<N*dim; i++)
  //  std::cout << i << " " << result_x[i] << " " << result_y[i] << std::endl;
}
