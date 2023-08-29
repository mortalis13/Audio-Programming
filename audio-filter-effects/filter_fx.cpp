#include "filter_fx.h"


double AudioFilter::processAudioSample(double xn) {
  // Biquad calc
  double yn = cf_a0 * xn + cf_a1 * x_z1 + cf_a2 * x_z2 - cf_b1 * y_z1 - cf_b2 * y_z2;

  checkFloatUnderflow(yn);

  x_z2 = x_z1;
  x_z1 = xn;

  y_z2 = y_z1;
  y_z1 = yn;

  return cf_d0 * xn + cf_c0 * yn;
}


void LowPassFilter::calculateFilterCoeffs() {
  double theta_c = 2.0 * kPi * fc / sampleRate;
  
  if (order == 1) {
    double gamma = cos(theta_c) / (1.0 + sin(theta_c));

    cf_a0 = (1.0 - gamma) / 2.0;
    cf_a1 = (1.0 - gamma) / 2.0;
    cf_a2 = 0.0;
    cf_b1 = -gamma;
    cf_b2 = 0.0;
  }
  else if (order == 2) {
    double d = 1.0 / Q;
    double betaNumerator = 1.0 - ((d / 2.0) * (sin(theta_c)));
    double betaDenominator = 1.0 + ((d / 2.0) * (sin(theta_c)));

    double beta = 0.5 * (betaNumerator / betaDenominator);
    double gamma = (0.5 + beta) * (cos(theta_c));
    double alpha = (0.5 + beta - gamma) / 2.0;

    cf_a0 = alpha;
    cf_a1 = 2.0 * alpha;
    cf_a2 = alpha;
    cf_b1 = -2.0 * gamma;
    cf_b2 = 2.0 * beta;
  }
}


void HighPassFilter::calculateFilterCoeffs() {
  double theta_c = 2.0 * kPi * fc / sampleRate;
  
  if (order == 1) {
    double gamma = cos(theta_c) / (1.0 + sin(theta_c));

    cf_a0 = (1.0 + gamma) / 2.0;
    cf_a1 = -(1.0 + gamma) / 2.0;
    cf_a2 = 0.0;
    cf_b1 = -gamma;
    cf_b2 = 0.0;
  }
  else if (order == 2) {
    double d = 1.0 / Q;

    double betaNumerator = 1.0 - ((d / 2.0) * (sin(theta_c)));
    double betaDenominator = 1.0 + ((d / 2.0) * (sin(theta_c)));

    double beta = 0.5 * (betaNumerator / betaDenominator);
    double gamma = (0.5 + beta) * (cos(theta_c));
    double alpha = (0.5 + beta + gamma) / 2.0;

    cf_a0 = alpha;
    cf_a1 = -2.0 * alpha;
    cf_a2 = alpha;
    cf_b1 = -2.0 * gamma;
    cf_b2 = 2.0 * beta;
  }
}


void BandFilter::calculateFilterCoeffs() {
  double K = tan(kPi * fc / sampleRate);
  double delta = K * K * Q + K + Q;
  
  if (type == BandPass) {
    cf_a0 = K / delta;;
    cf_a1 = 0.0;
    cf_a2 = -K / delta;
    cf_b1 = 2.0 * Q * (K * K - 1) / delta;
    cf_b2 = (K * K * Q - K + Q) / delta;
  }
  else if (type == BandStop) {
    cf_a0 = Q * (1 + K * K) / delta;
    cf_a1 = 2.0 * Q * (K * K - 1) / delta;
    cf_a2 = Q * (1 + K * K) / delta;
    cf_b1 = 2.0 * Q * (K * K - 1) / delta;
    cf_b2 = (K * K * Q - K + Q) / delta;
  }
}

void ShelfFilter::calculateFilterCoeffs() {
  double theta_c = 2.0 * kPi * fc / sampleRate;
  double mu = pow(10.0, db / 20.0);
  
  
  if (type == LowShelf) {
    double beta = 4.0 / (1.0 + mu);
    double delta = beta * tan(theta_c / 2.0);
    double gamma = (1.0 - delta) / (1.0 + delta);

    cf_a0 = (1.0 - gamma) / 2.0;
    cf_a1 = (1.0 - gamma) / 2.0;
    cf_a2 = 0.0;
    cf_b1 = -gamma;
    cf_b2 = 0.0;
    
    cf_c0 = mu - 1.0;
    cf_d0 = 1.0;
  }
  else if (type == HighShelf) {
    double beta = (1.0 + mu) / 4.0;
    double delta = beta * tan(theta_c / 2.0);
    double gamma = (1.0 - delta) / (1.0 + delta);

    cf_a0 = (1.0 + gamma) / 2.0;
    cf_a1 = -cf_a0;
    cf_a2 = 0.0;
    cf_b1 = -gamma;
    cf_b2 = 0.0;
    
    cf_c0 = mu - 1.0;
    cf_d0 = 1.0;
  }
}


void PeakingFilter::calculateFilterCoeffs() {
  if (!constQ) {
    // Non constant Q
    double theta_c = 2.0 * kPi * fc / sampleRate;
    double mu = pow(10.0, db / 20.0);

    double tanArg = theta_c / (2.0 * Q);
    if (tanArg >= 0.95 * kPi / 2.0) tanArg = 0.95 * kPi / 2.0;

    double zeta = 4.0 / (1.0 + mu);
    double betaNumerator = 1.0 - zeta * tan(tanArg);
    double betaDenominator = 1.0 + zeta * tan(tanArg);

    double beta = 0.5 * (betaNumerator / betaDenominator);
    double gamma = (0.5 + beta) * (cos(theta_c));
    double alpha = (0.5 - beta);

    cf_a0 = alpha;
    cf_a1 = 0.0;
    cf_a2 = -alpha;
    cf_b1 = -2.0 * gamma;
    cf_b2 = 2.0 * beta;

    cf_c0 = mu - 1.0;
    cf_d0 = 1.0;
  }
  else {
    // Constant Q
    double K = tan(kPi * fc / sampleRate);
    double Vo = pow(10.0, db / 20.0);

    double d0 = 1.0 + (1.0 / Q) * K + K * K;
    double e0 = 1.0 + (1.0 / (Vo * Q)) * K + K * K;
    double alpha = 1.0 + (Vo / Q) * K + K * K;
    double beta = 2.0 * (K * K - 1.0);
    double gamma = 1.0 - (Vo / Q) * K + K * K;
    double delta = 1.0 - (1.0 / Q) * K + K * K;
    double eta = 1.0 - (1.0 / (Vo * Q)) * K + K * K;
    
    if (db >= 0) {
      cf_a0 = alpha / d0;
      cf_a1 = beta  / d0;
      cf_a2 = gamma / d0;
      cf_b1 = beta  / d0;
      cf_b2 = delta / d0;
    }
    else {
      cf_a0 = d0    / e0;
      cf_a1 = beta  / e0;
      cf_a2 = delta / e0;
      cf_b1 = beta  / e0;
      cf_b2 = eta   / e0;
    }
  }
}
