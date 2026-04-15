#ifndef CHEM_CHANNEL_IINTERMEDIATEOBSERVER_H_
#define CHEM_CHANNEL_IINTERMEDIATEOBSERVER_H_

#include <memory>
#include <string>

#include "../models/signal.hpp"

/**
 *
 * Observer for the transmitter
 *
 *
 *
 */

class IntermediateObserver {
   public:
    virtual ~IntermediateObserver() {};
    virtual void Add2Buff(std::unique_ptr<chem::Signal> sig) = 0;
};

#endif  // CHEM_CHANNEL_IINTERMEDIATEOBSERVER_H_
