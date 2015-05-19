/* 
 * array/main.cpp 
 *
 * author(s): Karl Fuerlinger, LMU Munich */
/* @DASH_HEADER@ */

#include <unistd.h>
#include <iostream>
#include <libdash.h>

#define SIZE 10

using namespace std;

int main(int argc, char* argv[])
{
  dash::init(&argc, &argv);
  
  auto myid = dash::myid();
  auto size = dash::size();

  dash::Array<int> arr(SIZE);

  for( auto i=0; i<arr.local.size(); i++ ) {
    arr.local[i]=myid;
  }

  arr.barrier();
  
  if(myid==0 ) {  
    for( auto it = arr.begin(); it!=arr.end(); it++ ) {
      cout<<(*it)<<" ";
    }
    cout<<endl;
  }
  
  dash::finalize();
}