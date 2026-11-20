#pragma once

#include <Arduino.h>

void beginFirebaseAuth();
bool ensureFirebaseAuthentication();
void invalidateFirebaseAuthentication();
const String &firebaseIdToken();
