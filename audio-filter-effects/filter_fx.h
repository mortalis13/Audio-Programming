#ifndef FILTER_FX_H
#define FILTER_FX_H

#include <stdint.h>
#include <math.h>

const double kSmallestPositiveFloatValue = 1.175494351e-38;         /* min positive value */
const double kSmallestNegativeFloatValue = -1.175494351e-38;        /* min negative value */
const double kPi = 3.14159265358979323846264338327950288419716939937510582097494459230781640628620899;

const double DEFAULT_Q = 0.707;
const double MIN_FREQUENCY = 20.0;
const double MAX_FREQUENCY = 20480.0;
const double MIN_GAIN = -20.0;
const double MAX_GAIN = 20.0;


inline bool checkFloatUnderflow(double& value) {
  bool retValue = false;
  if (value > 0.0 && value < kSmallestPositiveFloatValue) {
    value = 0;
    retValue = true;
  }
  else if (value < 0.0 && value > kSmallestNegativeFloatValue) {
    value = 0;
    retValue = true;
  }
  return retValue;
}


class AudioFilter {
public:
  AudioFilter() {}
  virtual ~AudioFilter() = default;

  void reset() {
    resetStates();
  }

  void setSampleRate(double sampleRate) {
    this->sampleRate = sampleRate;
    calculateFilterCoeffs();
  }
  
  virtual void setQFactor(double qFactor) {
    if (qFactor <= 0) qFactor = DEFAULT_Q;
    this->Q = qFactor;
    calculateFilterCoeffs();
  }
  
  void setFrequency(double frequency) {
    frequency = fmin(fmax(frequency, MIN_FREQUENCY), MAX_FREQUENCY);
    if (this->fc == frequency) return;
    this->fc = frequency;
    calculateFilterCoeffs();
  }
  
  virtual void setGainDb(double db) {
    db = fmin(fmax(db, MIN_GAIN), MAX_GAIN);
    if (this->db == db) return;
    this->db = db;
    calculateFilterCoeffs();
  }
  
  double processAudioSample(double xn);

protected:
  virtual void calculateFilterCoeffs() = 0;
  
  void resetStates() {
    x_z1 = x_z2 = y_z1 = y_z2 = 0.0;
  }

protected:
  double cf_a0 = 0.0;
  double cf_a1 = 0.0;
  double cf_a2 = 0.0;
  double cf_b1 = 0.0;
  double cf_b2 = 0.0;

  double cf_c0 = 1.0;
  double cf_d0 = 0.0;
  
  double x_z1 = 0.0;
  double x_z2 = 0.0;
  double y_z1 = 0.0;
  double y_z2 = 0.0;
  
  double sampleRate = 44100.0;
  double Q = DEFAULT_Q;
  
  double fc = 100.0;
  double db = 0.0;
};


class LowPassFilter : public AudioFilter {
public:
  void setOrder(int order) {
    if (order != 1 && order != 2) order = 1;
    this->order = order;
    calculateFilterCoeffs();
  }
  virtual void setGainDb(double db) {/* N/A */}
protected:
  virtual void calculateFilterCoeffs();
private:
  int order = 1;
};


class HighPassFilter : public AudioFilter {
public:
  void setOrder(int order) {
    if (order != 1 && order != 2) order = 1;
    this->order = order;
    calculateFilterCoeffs();
  }
  virtual void setGainDb(double db) {/* N/A */}
protected:
  virtual void calculateFilterCoeffs();
private:
  int order = 1;
};


class BandFilter : public AudioFilter {
public:
  enum FilterType { BandPass, BandStop };
  void setType(FilterType type) {
    this->type = type;
    calculateFilterCoeffs();
  }
  virtual void setGainDb(double db) {/* N/A */}
protected:
  virtual void calculateFilterCoeffs();
private:
  FilterType type = BandPass;
};


class ShelfFilter : public AudioFilter {
public:
  enum FilterType { LowShelf, HighShelf };
  void setType(FilterType type) {
    this->type = type;
    calculateFilterCoeffs();
  }
  virtual void setQFactor(double qFactor) {/* N/A */}
protected:
  virtual void calculateFilterCoeffs();
private:
  FilterType type = LowShelf;
};


class PeakingFilter : public AudioFilter {
protected:
  virtual void calculateFilterCoeffs();
private:
  bool constQ = false;
};

#endif //FILTER_FX_H
