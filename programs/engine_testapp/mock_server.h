#pragma once

#include <memcached/engine_testapp.h>
#include <memcached/rbac.h>

void mock_init_alloc_hooks();

SERVER_HANDLE_V1* get_mock_server_api();

void init_mock_server();

void mock_time_travel(int by);

void mock_set_pre_link_function(PreLinkFunction function);

using CheckPrivilegeFunction =
        std::function<cb::rbac::PrivilegeAccess(gsl::not_null<const void*>,
                                                cb::rbac::Privilege,
                                                std::optional<ScopeID>,
                                                std::optional<CollectionID>)>;
void mock_set_check_privilege_function(CheckPrivilegeFunction function);
void mock_reset_check_privilege_function();
void mock_set_privilege_context_revision(uint32_t rev);
uint32_t mock_get_privilege_context_revision();
