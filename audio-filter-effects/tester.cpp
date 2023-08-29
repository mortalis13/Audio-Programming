#include <string>
#include <iostream>

#include "filter_fx.h"

using namespace std;

int main() {
  cout << "Start\n";
  
  LowPassFilter af;
  af.setSampleRate(44100);
  af.setFrequency(200);
  
  cout << "End\n";
  return 0;
}
