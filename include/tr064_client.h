#ifndef TR064_CLIENT_H
#define TR064_CLIENT_H

#include <Arduino.h>

struct Tr064Config {
  String username;
  String password;
  String number;
};

bool loadTr064Config(Tr064Config &config);
bool hasTr064Config(const Tr064Config &config);
bool triggerTr064Ring(const Tr064Config &config);

#endif // TR064_CLIENT_H
