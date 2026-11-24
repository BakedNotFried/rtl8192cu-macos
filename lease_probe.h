#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

size_t dhcp_discover(const uint8_t mac[6], uint32_t transaction, uint8_t *frame, size_t capacity);
bool dhcp_is_offer(const uint8_t *frame, size_t length, const uint8_t mac[6], uint32_t transaction);
