#pragma once

#include <Arduino.h>

void beginSupabaseAuth();
bool ensureSupabaseAuthentication();
void invalidateSupabaseAuthentication();
const String &supabaseAccessToken();
