/*
** Zabbix
** Copyright (C) 2001-2021 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "log.h"
#include "zbxself.h"
#include "zbxservice.h"
#include "zbxipcservice.h"
#include "service_manager.h"
#include "daemon.h"
#include "sighandler.h"
#include "dbcache.h"
#include "zbxalgo.h"
#include "zbxalgo.h"
#include "service_protocol.h"

extern unsigned char	process_type, program_type;
extern int		server_num, process_num;

typedef struct
{
	zbx_uint64_t		serviceid;
	zbx_uint64_t		current_eventid;
	int			status;
	int			old_status;
	int			algorithm;
	zbx_vector_ptr_t	service_problems;
	zbx_vector_ptr_t	service_problem_tags;
	zbx_vector_ptr_t	children;
	zbx_vector_ptr_t	parents;
}
zbx_service_t;

typedef struct
{
	zbx_uint64_t	eventid;
	zbx_uint64_t	service_problemid;
	zbx_uint64_t	serviceid;
	int		severity;
	int		clock;
}
zbx_service_problem_t;

typedef struct
{
	zbx_uint64_t		eventid;
	zbx_vector_ptr_t	services;
}
zbx_service_problem_index_t;

typedef struct
{
	zbx_uint64_t	service_problem_tagid;
	zbx_uint64_t	current_eventid;
	zbx_service_t	*service;
	char		*tag;
	int		operator;
	char		*value;
}
zbx_service_problem_tag_t;

typedef struct
{
	char		*tag;
	zbx_hashset_t	values;
}
zbx_tag_services_t;

typedef struct
{
	char			*value;
	zbx_vector_ptr_t	service_problem_tags;
}
zbx_values_eq_t;

typedef struct
{
	zbx_uint64_t	linkid;
	zbx_service_t	*parent;
	zbx_service_t	*child;
}
zbx_services_link_t;

typedef struct
{
	zbx_uint64_t		serviceid;
	zbx_vector_ptr_t	service_problems;
	zbx_vector_ptr_t	service_problems_recovered;
#define ZBX_FLAG_SERVICE_RECALCULATE	__UINT64_C(0x01)
#define ZBX_FLAG_SERVICE_DELETE		__UINT64_C(0x02)
	int			flags;
}
zbx_services_diff_t;

/* preprocessing manager data */
typedef struct
{
	zbx_hashset_t	services;
	zbx_hashset_t	services_diff;
	zbx_hashset_t	service_problem_tags;
	zbx_hashset_t	service_problem_tags_index;
	zbx_hashset_t	services_links;
	zbx_hashset_t	service_problems_index;
	zbx_hashset_t	problem_events;
	zbx_hashset_t	recovery_events;

}
zbx_service_manager_t;

/*#define ZBX_AVAILABILITY_MANAGER_DELAY		1*/
#define ZBX_SERVICE_MANAGER_SYNC_DELAY_SEC		5

static void	event_clean(zbx_event_t *event)
{
	zbx_vector_ptr_clear_ext(&event->tags, (zbx_clean_func_t)zbx_free_tag);
	zbx_vector_ptr_destroy(&event->tags);
	zbx_free(event);
}

static void	event_ptr_clean(zbx_event_t **event)
{
	event_clean(*event);
}

static zbx_hash_t	default_uint64_ptr_hash_func(const void *d)
{
	return ZBX_DEFAULT_UINT64_HASH_FUNC(*(const zbx_uint64_t **)d);
}

static void	match_event_to_service_problem_tags(zbx_event_t *event, zbx_hashset_t *service_problem_tags_index,
		zbx_hashset_t *services_diffs, int flags)
{
	int			i, j;
	zbx_vector_ptr_t	candidates;

	if (TRIGGER_SEVERITY_NOT_CLASSIFIED == event->severity)
		return;

	zbx_vector_ptr_create(&candidates);

	for (i = 0; i < event->tags.values_num; i++)
	{
		zbx_tag_services_t	tag_services, *ptag_services;
		const zbx_tag_t		*tag = (const zbx_tag_t *)event->tags.values[i];

		tag_services.tag = tag->tag;

		if (NULL != (ptag_services = (zbx_tag_services_t *)zbx_hashset_search(service_problem_tags_index,
				&tag_services)))
		{
			zbx_values_eq_t	values_eq = {.value = tag->value}, *pvalues_eq;

			if (NULL != (pvalues_eq = (zbx_values_eq_t *)zbx_hashset_search(&ptag_services->values,
					&values_eq)))
			{
				for (j = 0; j < pvalues_eq->service_problem_tags.values_num; j++)
				{
					zbx_service_problem_tag_t	*service_problem_tag;

					service_problem_tag = (zbx_service_problem_tag_t *)pvalues_eq->service_problem_tags.values[j];

					service_problem_tag->current_eventid = event->eventid;

					if (service_problem_tag->service->current_eventid != event->eventid ||
							0 == candidates->values_num)
					{
						service_problem_tag->service->current_eventid = event->eventid;
						zbx_vector_ptr_append(&candidates, service_problem_tag->service);
					}
				}
			}
		}
	}

	for (i = 0; i < candidates.values_num; i++)
	{
		zbx_service_t	*service = (zbx_service_t *)candidates.values[i];

		for (j = 0; j < service->service_problem_tags.values_num; j++)
		{
			zbx_service_problem_tag_t	*service_problem_tag;

			service_problem_tag = (zbx_service_problem_tag_t *)service->service_problem_tags.values[j];

			if (service->current_eventid != service_problem_tag->current_eventid)
				break;
		}

		if (j == service->service_problem_tags.values_num)
		{
			zbx_services_diff_t	services_diff = {.serviceid = service->serviceid}, *pservices_diff;
			zbx_service_problem_t	*service_problem;

			if (NULL == (pservices_diff = zbx_hashset_search(services_diffs, &services_diff)))
			{
				zbx_vector_ptr_create(&services_diff.service_problems);
				zbx_vector_ptr_create(&services_diff.service_problems_recovered);
				services_diff.flags = flags;
				pservices_diff = zbx_hashset_insert(services_diffs, &services_diff, sizeof(services_diff));
			}

			service_problem = zbx_malloc(NULL, sizeof(zbx_service_problem_t));
			service_problem->eventid = event->eventid;
			service_problem->serviceid = pservices_diff->serviceid;
			service_problem->severity = event->severity;
			service_problem->clock = event->clock;

			zbx_vector_ptr_append(&pservices_diff->service_problems, service_problem);
		}
	}

	zbx_vector_ptr_destroy(&candidates);
}

static void	db_get_events(zbx_hashset_t *problem_events)
{
	DB_RESULT	result;
	zbx_event_t	*event = NULL;
	DB_ROW		row;
	zbx_uint64_t	eventid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	result = DBselect("select p.eventid,p.clock,p.severity,t.tag,t.value"
			" from problem p"
			" left join problem_tag t"
				" on p.eventid=t.eventid"
			" where p.source=%d"
				" and p.object=%d"
				" and r_eventid is null"
			" order by p.eventid",
			EVENT_SOURCE_TRIGGERS, EVENT_OBJECT_TRIGGER);

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(eventid, row[0]);

		if (NULL == event || eventid != event->eventid)
		{
			event = (zbx_event_t *)zbx_malloc(NULL, sizeof(zbx_event_t));

			event->eventid = eventid;
			event->clock = atoi(row[1]);
			event->value = TRIGGER_VALUE_PROBLEM;
			event->severity = atoi(row[2]);
			zbx_vector_ptr_create(&event->tags);
			zbx_hashset_insert(problem_events, &event, sizeof(zbx_event_t **));
		}

		if (FAIL == DBis_null(row[3]))
		{
			zbx_tag_t	*tag;

			tag = (zbx_tag_t *)zbx_malloc(NULL, sizeof(zbx_tag_t));
			tag->tag = zbx_strdup(NULL, row[3]);
			tag->value = zbx_strdup(NULL, row[4]);
			zbx_vector_ptr_append(&event->tags, tag);
		}
	}
	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

static void	sync_services(zbx_hashset_t *services, int *updated)
{
	DB_RESULT	result;
	DB_ROW		row;
	zbx_service_t	service, *pservice;

	result = DBselect("select serviceid,status,algorithm from services");

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(service.serviceid, row[0]);
		service.status = atoi(row[1]);
		service.algorithm = atoi(row[2]);

		if (NULL == (pservice = zbx_hashset_search(services, &service)))
		{
			service.current_eventid = 0;
			zbx_vector_ptr_create(&service.children);
			zbx_vector_ptr_create(&service.parents);
			zbx_vector_ptr_create(&service.service_problem_tags);
			zbx_vector_ptr_create(&service.service_problems);

			zbx_hashset_insert(services, &service, sizeof(service));
			(*updated)++;
			continue;
		}

		if (pservice->status != service.status || pservice->algorithm != service.algorithm)
		{
			pservice->status = service.status;
			pservice->algorithm = service.algorithm;
			(*updated)++;
		}
	}
	DBfree_result(result);
}

static zbx_hash_t	values_eq_hash(const void *data)
{
	zbx_values_eq_t	*d = (zbx_values_eq_t *)data;

	return ZBX_DEFAULT_STRING_HASH_ALGO(d->value, strlen(d->value), ZBX_DEFAULT_HASH_SEED);
}

static int	values_eq_compare(const void *d1, const void *d2)
{
	return strcmp(((zbx_values_eq_t *)d1)->value, ((zbx_values_eq_t *)d2)->value);
}

static void	values_eq_clean(void *data)
{
	zbx_values_eq_t	*d = (zbx_values_eq_t *)data;

	zbx_vector_ptr_destroy(&d->service_problem_tags);
	zbx_free(d->value);
}

static void	sync_service_problem_tags(zbx_service_manager_t *service_manager)
{
	DB_RESULT			result;
	DB_ROW				row;
	zbx_service_problem_tag_t	service_problem_tag, *pservice_problem_tag;

	result = DBselect("select service_problem_tagid,serviceid,tag,operator,value from service_problem_tag");

	while (NULL != (row = DBfetch(result)))
	{
		zbx_uint64_t		serviceid;
		zbx_service_t		*pservice;
		zbx_tag_services_t	tag_services, *ptag_services;
		zbx_values_eq_t		value_eq, *pvalue_eq;

		ZBX_STR2UINT64(service_problem_tag.service_problem_tagid, row[0]);
		ZBX_STR2UINT64(serviceid, row[1]);
		pservice = zbx_hashset_search(&service_manager->services, &serviceid);

		if (NULL == (pservice_problem_tag = zbx_hashset_search(&service_manager->service_problem_tags,
				&service_problem_tag)))
		{
			service_problem_tag.current_eventid = 0;
			service_problem_tag.tag = zbx_strdup(NULL, row[2]);
			service_problem_tag.operator = atoi(row[3]);
			service_problem_tag.value = zbx_strdup(NULL, row[4]);
			service_problem_tag.service = pservice;

			pservice_problem_tag = zbx_hashset_insert(&service_manager->service_problem_tags,
					&service_problem_tag, sizeof(service_problem_tag));

			if (NULL != service_problem_tag.service)
				zbx_vector_ptr_append(&pservice->service_problem_tags, pservice_problem_tag);

			tag_services.tag = service_problem_tag.tag;

			/* add tag to index */
			if (NULL == (ptag_services = zbx_hashset_search(&service_manager->service_problem_tags_index,
					&tag_services)))
			{
				tag_services.tag = zbx_strdup(NULL, service_problem_tag.tag);

				zbx_hashset_create_ext(&tag_services.values, 1,
						values_eq_hash, values_eq_compare,
						values_eq_clean, ZBX_DEFAULT_MEM_MALLOC_FUNC,
						ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

				ptag_services = zbx_hashset_insert(&service_manager->service_problem_tags_index,
						&tag_services, sizeof(tag_services));
			}

			/* add value to index */
			value_eq.value = service_problem_tag.value;
			if (NULL == (pvalue_eq = zbx_hashset_search(&ptag_services->values, &value_eq)))
			{
				value_eq.value = zbx_strdup(NULL, service_problem_tag.value);
				zbx_vector_ptr_create(&value_eq.service_problem_tags);
				zbx_vector_ptr_append(&value_eq.service_problem_tags, pservice_problem_tag);

				pvalue_eq = zbx_hashset_insert(&ptag_services->values, &value_eq, sizeof(value_eq));
			}

			continue;
		}

		/* handle existing tag being moved to other service */
		if (NULL != pservice_problem_tag->service && pservice_problem_tag->service != pservice)
		{
			int	index;

			THIS_SHOULD_NEVER_HAPPEN;

			index = zbx_vector_ptr_search(&pservice_problem_tag->service->service_problem_tags,
					pservice_problem_tag, ZBX_DEFAULT_PTR_COMPARE_FUNC);

			if (FAIL == index)
				THIS_SHOULD_NEVER_HAPPEN;
			else
				zbx_vector_ptr_remove(&pservice_problem_tag->service->service_problem_tags, index);

			if (NULL != pservice)
				zbx_vector_ptr_append(&pservice->service_problem_tags, pservice_problem_tag);

			pservice_problem_tag->service = pservice;
		}

		pservice_problem_tag->tag = zbx_strdup(pservice_problem_tag->tag, row[2]);
		pservice_problem_tag->operator = atoi(row[3]);
		pservice_problem_tag->value = zbx_strdup(pservice_problem_tag->value, row[4]);
	}
	DBfree_result(result);
}

static void	sync_services_links(zbx_service_manager_t *service_manager)
{
	DB_RESULT	result;
	DB_ROW		row;

	result = DBselect("select linkid,serviceupid,servicedownid from services_links");

	while (NULL != (row = DBfetch(result)))
	{
		zbx_uint64_t		serviceupid, servicedownid;
		zbx_services_link_t	services_link, *pservices_link;

		ZBX_STR2UINT64(services_link.linkid, row[0]);
		ZBX_STR2UINT64(serviceupid, row[1]);
		ZBX_STR2UINT64(servicedownid, row[2]);

		if (NULL == (pservices_link = zbx_hashset_search(&service_manager->services_links, &services_link)))
		{
			services_link.parent = zbx_hashset_search(&service_manager->services, &serviceupid);
			services_link.child = zbx_hashset_search(&service_manager->services, &servicedownid);

			if (NULL == services_link.parent || NULL == services_link.child)
			{
				/* it is not possible for link to exist without corresponding service */
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}

			zbx_vector_ptr_append(&services_link.parent->children, services_link.child);
			zbx_vector_ptr_append(&services_link.child->parents, services_link.parent);
			zbx_hashset_insert(&service_manager->services_links, &services_link, sizeof(services_link));
			continue;
		}

		/* links cannot be changed */

		/* TODO: handle deleted links by removing child and parent from services vector */
	}
	DBfree_result(result);
}

static void	index_service_problem(zbx_service_t *service, zbx_hashset_t *service_problems_index,
		zbx_service_problem_t *pservice_problem)
{
	zbx_service_problem_index_t	*pservice_problem_index, service_problem_index;

	service_problem_index.eventid = pservice_problem->eventid;
	if (NULL == (pservice_problem_index = zbx_hashset_search(service_problems_index, &service_problem_index)))
	{
		zbx_vector_ptr_create(&service_problem_index.services);
		pservice_problem_index = zbx_hashset_insert(service_problems_index, &service_problem_index,
				sizeof(service_problem_index));
	}

	zbx_vector_ptr_append(&pservice_problem_index->services, service);

	zbx_vector_ptr_append(&service->service_problems, pservice_problem);
}

static void	remove_service_problem(zbx_service_t *service, int index, zbx_hashset_t *service_problems_index)
{
	zbx_service_problem_index_t	*pservice_problem_index, service_problem_index;
	int				i;
	zbx_service_problem_t		*pservice_problem;

	pservice_problem = (zbx_service_problem_t *)service->service_problems.values[index];

	service_problem_index.eventid = pservice_problem->eventid;
	if (NULL == (pservice_problem_index = zbx_hashset_search(service_problems_index, &service_problem_index)))
	{
		THIS_SHOULD_NEVER_HAPPEN;
	}
	else
	{
		if (FAIL == (i = zbx_vector_ptr_search(&pservice_problem_index->services, service,
				ZBX_DEFAULT_PTR_COMPARE_FUNC)))
		{
			THIS_SHOULD_NEVER_HAPPEN;
		}
		else
		{
			zbx_vector_ptr_remove_noorder(&pservice_problem_index->services, i);

			if (0 == pservice_problem_index->services.values_num)
				zbx_hashset_remove_direct(service_problems_index, pservice_problem_index);
		}
	}

	zbx_vector_ptr_remove_noorder(&service->service_problems, index);
	zbx_free(pservice_problem);

}

static void	sync_service_problems(zbx_hashset_t *services, zbx_hashset_t *service_problems_index)
{
	DB_RESULT	result;
	DB_ROW		row;
	zbx_service_t	service, *pservice;

	result = DBselect("select service_problemid,eventid,serviceid,severity from service_problem");

	while (NULL != (row = DBfetch(result)))
	{
		zbx_service_problem_t	*pservice_problem;

		ZBX_STR2UINT64(service.serviceid, row[2]);

		if (NULL == (pservice = zbx_hashset_search(services, &service)))
		{
			THIS_SHOULD_NEVER_HAPPEN;
			continue;
		}

		pservice_problem = zbx_malloc(NULL, sizeof(zbx_service_problem_t));
		ZBX_STR2UINT64(pservice_problem->service_problemid, row[0]);
		ZBX_STR2UINT64(pservice_problem->eventid, row[1]);
		pservice_problem->serviceid = service.serviceid;
		pservice_problem->severity = atoi(row[3]);
		pservice_problem->clock = 0;

		index_service_problem(pservice, service_problems_index, pservice_problem);
	}
	DBfree_result(result);
}

static void	service_clean(zbx_service_t *service)
{
	zbx_vector_ptr_destroy(&service->children);
	zbx_vector_ptr_destroy(&service->parents);
	zbx_vector_ptr_destroy(&service->service_problem_tags);
	zbx_vector_ptr_clear_ext(&service->service_problems, zbx_ptr_free);
	zbx_vector_ptr_destroy(&service->service_problems);

}

static void	service_problem_tag_clean(zbx_service_problem_tag_t *service_problem_tag)
{
	zbx_free(service_problem_tag->tag);
	zbx_free(service_problem_tag->value);
}

static void	service_diff_clean(void *data)
{
	zbx_services_diff_t	*d = (zbx_services_diff_t *)data;

	zbx_vector_ptr_clear_ext(&d->service_problems_recovered, zbx_ptr_free);
	zbx_vector_ptr_clear_ext(&d->service_problems, zbx_ptr_free);
	zbx_vector_ptr_destroy(&d->service_problems);
	zbx_vector_ptr_destroy(&d->service_problems_recovered);
}

static zbx_hash_t	tag_services_hash(const void *data)
{
	zbx_tag_services_t	*d = (zbx_tag_services_t *)data;

	return ZBX_DEFAULT_STRING_HASH_ALGO(d->tag, strlen(d->tag), ZBX_DEFAULT_HASH_SEED);
}

static int	tag_services_compare(const void *d1, const void *d2)
{
	return strcmp(((zbx_tag_services_t *)d1)->tag, ((zbx_tag_services_t *)d2)->tag);
}

static void	tag_services_clean(void *data)
{
	zbx_tag_services_t	*d = (zbx_tag_services_t *)data;

	zbx_hashset_destroy(&d->values);
	zbx_free(d->tag);
}

static void	service_problems_index_clean(void *data)
{
	zbx_service_problem_index_t	*d = (zbx_service_problem_index_t *)data;

	zbx_vector_ptr_destroy(&d->services);
}

/* status update queue items */
typedef struct
{
	/* the update source id */
	zbx_uint64_t	sourceid;
	/* the new status */
	int		status;
	/* timestamp */
	int		clock;
}
zbx_status_update_t;

/* status update queue items */
typedef struct
{
	zbx_uint64_t	serviceid;
	int		old_status;
	int		status;
}
zbx_service_update_t;

/******************************************************************************
 *                                                                            *
 * Function: its_updates_append                                               *
 *                                                                            *
 * Purpose: adds an update to the queue                                       *
 *                                                                            *
 * Parameters: updates   - [OUT] the update queue                             *
 *             sourceid  - [IN] the update source id                          *
 *             status    - [IN] the update status                             *
 *             clock     - [IN] the update timestamp                          *
 *                                                                            *
 ******************************************************************************/
static void	its_updates_append(zbx_vector_ptr_t *updates, zbx_uint64_t sourceid, int status, int clock)
{
	zbx_status_update_t	*update;

	update = (zbx_status_update_t *)zbx_malloc(NULL, sizeof(zbx_status_update_t));

	update->sourceid = sourceid;
	update->status = status;
	update->clock = clock;

	zbx_vector_ptr_append(updates, update);
}

static void	update_service(zbx_hashset_t *service_updates, zbx_uint64_t serviceid, int old_status, int status)
{
	zbx_service_update_t	update = {.serviceid = serviceid, .old_status = old_status, .status = status}, *pupdate;

	if (NULL == (pupdate = (zbx_service_update_t *)zbx_hashset_search(service_updates, &update)))
		pupdate = (zbx_service_update_t *)zbx_hashset_insert(service_updates, &update, sizeof(update));

	pupdate->status = status;
}

/******************************************************************************
 *                                                                            *
 * Function: its_updates_compare                                              *
 *                                                                            *
 * Purpose: used to sort service updates by source id                         *
 *                                                                            *
 ******************************************************************************/
static int	its_updates_compare(const zbx_status_update_t **update1, const zbx_status_update_t **update2)
{
	ZBX_RETURN_IF_NOT_EQUAL((*update1)->sourceid, (*update2)->sourceid);

	return 0;
}

/******************************************************************************
 *                                                                            *
 * Function: its_write_status_and_alarms                                      *
 *                                                                            *
 * Purpose: writes service status changes and generated service alarms into   *
 *          database                                                          *
 *                                                                            *
 * Parameters: itservices - [IN] the services data                            *
 *             alarms     - [IN] the service alarms update queue              *
 *                                                                            *
 * Return value: SUCCEED - the data was written successfully                  *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	its_write_status_and_alarms(zbx_vector_ptr_t *alarms, zbx_hashset_t *service_updates,
		zbx_vector_ptr_t *service_problems_new, zbx_vector_uint64_t *service_problemids)
{
	int			i, ret = FAIL;
	zbx_vector_ptr_t	updates;
	char			*sql = NULL;
	size_t			sql_alloc = 0, sql_offset = 0;
	zbx_uint64_t		alarmid;
	zbx_hashset_iter_t	iter;
	zbx_service_update_t	*itservice;

	/* get a list of service status updates that must be written to database */
	zbx_vector_ptr_create(&updates);

	zbx_hashset_iter_reset(service_updates, &iter);
	while (NULL != (itservice = (zbx_service_update_t *)zbx_hashset_iter_next(&iter)))
	{
		if (itservice->old_status != itservice->status)
			its_updates_append(&updates, itservice->serviceid, itservice->status, 0);
	}

	/* write service status changes into database */
	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (0 != updates.values_num)
	{
		zbx_vector_ptr_sort(&updates, (zbx_compare_func_t)its_updates_compare);

		for (i = 0; i < updates.values_num; i++)
		{
			zbx_status_update_t	*update = (zbx_status_update_t *)updates.values[i];

			zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
					"update services"
					" set status=%d"
					" where serviceid=" ZBX_FS_UI64 ";\n",
					update->status, update->sourceid);

			if (SUCCEED != DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset))
				goto out;
		}
	}

	zbx_vector_uint64_sort(service_problemids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	if (0 != service_problemids->values_num)
	{
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "delete from service_problem where");
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "service_problemid", service_problemids->values,
				service_problemids->values_num);
		zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, ";\n");
	}

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset);

	if (16 < sql_offset)
	{
		if (ZBX_DB_OK > DBexecute("%s", sql))
			goto out;
	}

	ret = SUCCEED;

	/* write generated service alarms into database */
	if (0 != alarms->values_num)
	{
		zbx_db_insert_t	db_insert;

		alarmid = DBget_maxid_num("service_alarms", alarms->values_num);

		zbx_db_insert_prepare(&db_insert, "service_alarms", "servicealarmid", "serviceid", "value", "clock",
				NULL);

		for (i = 0; i < alarms->values_num; i++)
		{
			zbx_status_update_t	*update = (zbx_status_update_t *)alarms->values[i];

			zbx_db_insert_add_values(&db_insert, alarmid++, update->sourceid, update->status,
					update->clock);
		}

		ret = zbx_db_insert_execute(&db_insert);

		zbx_db_insert_clean(&db_insert);
	}

	if (0 != service_problems_new->values_num)
	{
		zbx_db_insert_t	db_insert;
		zbx_uint64_t	service_problemid;

		service_problemid = DBget_maxid_num("service_problem", service_problems_new->values_num);

		zbx_db_insert_prepare(&db_insert, "service_problem", "service_problemid", "eventid", "serviceid",
				"severity", NULL);

		zbx_vector_ptr_sort(service_problems_new, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

		for (i = 0; i < service_problems_new->values_num; i++)
		{
			zbx_service_problem_t	*service_problem;

			service_problem = (zbx_service_problem_t *)service_problems_new->values[i];
			service_problem->service_problemid = service_problemid++;
			zbx_db_insert_add_values(&db_insert, service_problem->service_problemid,
					service_problem->eventid, service_problem->serviceid,
					service_problem->severity);
		}

		ret = zbx_db_insert_execute(&db_insert);

		zbx_db_insert_clean(&db_insert);
	}
out:
	zbx_free(sql);

	zbx_vector_ptr_clear_ext(&updates, zbx_ptr_free);
	zbx_vector_ptr_destroy(&updates);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: its_itservice_update_status                                      *
 *                                                                            *
 * Purpose: updates service and its parents statuses                          *
 *                                                                            *
 * Parameters: service    - [IN] the service to update                        *
 *             clock      - [IN] the update timestamp                         *
 *             alarms     - [OUT] the alarms update queue                     *
 *                                                                            *
 * Comments: This function recalculates service status according to the       *
 *           algorithm and status of the children services. If the status     *
 *           has been changed, an alarm is generated and parent services      *
 *           (up until the root service) are updated too.                     *
 *                                                                            *
 ******************************************************************************/
static void	its_itservice_update_status(zbx_service_t *itservice, int clock, zbx_vector_ptr_t *alarms,
		zbx_hashset_t *service_updates)
{
	int	status, i;

	switch (itservice->algorithm)
	{
		case SERVICE_ALGORITHM_MIN:
			status = TRIGGER_SEVERITY_COUNT;
			for (i = 0; i < itservice->children.values_num; i++)
			{
				zbx_service_t	*child = (zbx_service_t *)itservice->children.values[i];

				if (child->status < status)
					status = child->status;
			}
			break;
		case SERVICE_ALGORITHM_MAX:
			status = 0;
			for (i = 0; i < itservice->children.values_num; i++)
			{
				zbx_service_t	*child = (zbx_service_t *)itservice->children.values[i];

				if (child->status > status)
					status = child->status;
			}
			break;
		case SERVICE_ALGORITHM_NONE:
			goto out;
		default:
			zabbix_log(LOG_LEVEL_ERR, "unknown calculation algorithm of service status [%d]",
					itservice->algorithm);
			goto out;
	}

	if (itservice->status != status)
	{
		update_service(service_updates, itservice->serviceid, itservice->status, status);
		itservice->status = status;

		its_updates_append(alarms, itservice->serviceid, status, clock);

		/* update parent services */
		for (i = 0; i < itservice->parents.values_num; i++)
			its_itservice_update_status((zbx_service_t *)itservice->parents.values[i], clock, alarms, service_updates);
	}
out:
	;
}

static void	db_update_services(zbx_hashset_t *services, zbx_hashset_t *services_diff,
		zbx_hashset_t *service_problems_index)
{
	zbx_hashset_iter_t	iter;
	zbx_services_diff_t	*pservices_diff;
	zbx_vector_ptr_t	alarms, service_problems_new;
	zbx_vector_uint64_t	service_problemids;
	zbx_hashset_t		service_updates;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_vector_ptr_create(&alarms);
	zbx_vector_ptr_create(&service_problems_new);
	zbx_vector_uint64_create(&service_problemids);
	zbx_hashset_create(&service_updates, 100, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zbx_hashset_iter_reset(services_diff, &iter);
	while (NULL != (pservices_diff = (zbx_services_diff_t *)zbx_hashset_iter_next(&iter)))
	{
		zbx_service_t	service = {.serviceid = pservices_diff->serviceid}, *pservice;
		int		status = TRIGGER_SEVERITY_NOT_CLASSIFIED, i, clock = 0;

		pservice = zbx_hashset_search(services, &service);

		if (NULL == pservice)
		{
			THIS_SHOULD_NEVER_HAPPEN;
			continue;
		}

		if (pservices_diff->flags & ZBX_FLAG_SERVICE_DELETE)
		{
			for (i = 0; i < pservice->service_problems.values_num; i++)
			{
				zbx_service_problem_t	*service_problem;
				int			index;

				service_problem = (zbx_service_problem_t *)pservice->service_problems.values[i];

				if (FAIL == (index = zbx_vector_ptr_search(&pservices_diff->service_problems,
						&service_problem->eventid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
				{
					zbx_vector_uint64_append(&service_problemids, service_problem->service_problemid);
					remove_service_problem(pservice, i, service_problems_index);
					i--;
					continue;
				}

				zbx_free(pservices_diff->service_problems.values[index]);
				zbx_vector_ptr_remove_noorder(&pservices_diff->service_problems, index);
			}
		}

		for (i = 0; i < pservices_diff->service_problems.values_num; i++)
		{
			zbx_service_problem_t	*service_problem;

			service_problem = (zbx_service_problem_t *)pservices_diff->service_problems.values[i];
			zbx_vector_ptr_remove_noorder(&pservices_diff->service_problems, i);
			i--;
			index_service_problem(pservice, service_problems_index, service_problem);
			zbx_vector_ptr_append(&service_problems_new, service_problem);
		}

		for (i = 0; i < pservices_diff->service_problems_recovered.values_num; i++)
		{
			zbx_service_problem_t	*service_problem;
			int			index;

			service_problem = (zbx_service_problem_t *)pservices_diff->service_problems_recovered.values[i];

			if (FAIL == (index = zbx_vector_ptr_search(&pservice->service_problems,
					&service_problem->eventid, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC)))
			{
				THIS_SHOULD_NEVER_HAPPEN;
				continue;
			}
			else
			{
				if (clock < service_problem->clock)
					clock = service_problem->clock;

				service_problem = (zbx_service_problem_t *)pservice->service_problems.values[index];
				zbx_vector_uint64_append(&service_problemids, service_problem->service_problemid);

				remove_service_problem(pservice, index, service_problems_index);
			}
		}

		for (i = 0; i < pservice->service_problems.values_num; i++)
		{
			zbx_service_problem_t	*service_problem;

			service_problem = (zbx_service_problem_t *)pservice->service_problems.values[i];

			if (service_problem->severity > status)
			{
				status = service_problem->severity;
				clock = service_problem->clock;
			}
		}

		if (0 == clock)
			clock = time(NULL);

		if (pservice->status == status || SERVICE_ALGORITHM_NONE == pservice->algorithm)
			continue;

		update_service(&service_updates, pservice->serviceid, pservice->status, status);
		pservice->status = status;

		its_updates_append(&alarms, pservice->serviceid, pservice->status, clock);

		/* update parent services */
		for (i = 0; i < pservice->parents.values_num; i++)
		{
			its_itservice_update_status((zbx_service_t *)pservice->parents.values[i], clock, &alarms,
					&service_updates);
		}
	}

	do
	{
		DBbegin();
		its_write_status_and_alarms(&alarms, &service_updates, &service_problems_new, &service_problemids);
	}
	while (ZBX_DB_DOWN == DBcommit());


	zbx_vector_uint64_destroy(&service_problemids);
	zbx_vector_ptr_destroy(&service_problems_new);
	zbx_hashset_destroy(&service_updates);
	zbx_vector_ptr_clear_ext(&alarms, zbx_ptr_free);
	zbx_vector_ptr_destroy(&alarms);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

static void	process_events(zbx_vector_ptr_t *events, zbx_service_manager_t *service_manager)
{
	int	i;

	for (i = 0; i < events->values_num; i++)
	{
		zbx_event_t			*event, **ptr;
		zbx_service_problem_index_t	*pservice_problem_index, service_problem_index;

		event = events->values[i];

		switch (event->value)
		{
			case TRIGGER_VALUE_OK:
				if (NULL == (ptr = zbx_hashset_search(&service_manager->problem_events, &event)))
				{
					/* handle possible race condition when recovery is received before problem */
					zbx_hashset_insert(&service_manager->recovery_events, &event, sizeof(zbx_event_t **));
					continue;
				}

				service_problem_index.eventid = event->eventid;
				if (NULL != (pservice_problem_index = zbx_hashset_search(
						&service_manager->service_problems_index, &service_problem_index)))
				{
					int	j;

					for (j = 0; j < pservice_problem_index->services.values_num; j++)
					{
						zbx_service_t		*service;
						zbx_services_diff_t	services_diff, *pservices_diff;
						zbx_service_problem_t	*service_problem;

						service = (zbx_service_t *)pservice_problem_index->services.values[j];
						services_diff.serviceid = service->serviceid;
						if (NULL == (pservices_diff = zbx_hashset_search(&service_manager->services_diff, &services_diff)))
						{
							zbx_vector_ptr_create(&services_diff.service_problems);
							zbx_vector_ptr_create(&services_diff.service_problems_recovered);
							services_diff.flags = ZBX_FLAG_SERVICE_RECALCULATE;
							pservices_diff = zbx_hashset_insert(&service_manager->services_diff, &services_diff, sizeof(services_diff));
						}

						service_problem = zbx_malloc(NULL, sizeof(zbx_service_problem_t));
						service_problem->eventid = event->eventid;
						service_problem->serviceid = pservices_diff->serviceid;
						service_problem->severity = event->severity;
						service_problem->clock = event->clock;

						zbx_vector_ptr_append(&pservices_diff->service_problems_recovered, service_problem);
					}
				}

				event_clean(event);
				zbx_hashset_remove_direct(&service_manager->problem_events, ptr);
				break;
			case TRIGGER_VALUE_PROBLEM:
				if (NULL != (ptr = zbx_hashset_search(&service_manager->problem_events, &event)))
				{
					zabbix_log(LOG_LEVEL_ERR, "cannot process event \"" ZBX_FS_UI64 "\": event"
							" already processed", event->eventid);
					THIS_SHOULD_NEVER_HAPPEN;
					event_clean(event);
					continue;
				}

				if (NULL != (ptr = zbx_hashset_search(&service_manager->recovery_events, &event)))
				{
					/* handle possible race condition when recovery is received before problem */
					zbx_hashset_remove_direct(&service_manager->recovery_events, ptr);
					event_clean(event);
					continue;
				}

				zbx_hashset_insert(&service_manager->problem_events, &event, sizeof(zbx_event_t **));

				match_event_to_service_problem_tags(event,
						&service_manager->service_problem_tags_index,
						&service_manager->services_diff,
						ZBX_FLAG_SERVICE_RECALCULATE);

				break;
			default:
				zabbix_log(LOG_LEVEL_ERR, "cannot process event \"" ZBX_FS_UI64 "\" unexpected value:%d",
						event->eventid, event->value);
				THIS_SHOULD_NEVER_HAPPEN;
				event_clean(event);
		}
	}

	db_update_services(&service_manager->services, &service_manager->services_diff,
			&service_manager->service_problems_index);
	zbx_hashset_clear(&service_manager->services_diff);
}


static void	service_manager_init(zbx_service_manager_t *service_manager)
{
	zbx_hashset_create_ext(&service_manager->problem_events, 1000, default_uint64_ptr_hash_func,
			zbx_default_uint64_ptr_compare_func, (zbx_clean_func_t)event_ptr_clean,
			ZBX_DEFAULT_MEM_MALLOC_FUNC, ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->recovery_events, 1, default_uint64_ptr_hash_func,
			zbx_default_uint64_ptr_compare_func, (zbx_clean_func_t)event_ptr_clean,
			ZBX_DEFAULT_MEM_MALLOC_FUNC, ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->services, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC, (zbx_clean_func_t)service_clean,
			ZBX_DEFAULT_MEM_MALLOC_FUNC, ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->service_problem_tags, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC, (zbx_clean_func_t)service_problem_tag_clean,
			ZBX_DEFAULT_MEM_MALLOC_FUNC, ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->service_problem_tags_index, 1000, tag_services_hash,
			tag_services_compare, tag_services_clean, ZBX_DEFAULT_MEM_MALLOC_FUNC,
			ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->service_problems_index, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC, service_problems_index_clean, ZBX_DEFAULT_MEM_MALLOC_FUNC,
			ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->services_links, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC, NULL, ZBX_DEFAULT_MEM_MALLOC_FUNC,
			ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);

	zbx_hashset_create_ext(&service_manager->services_diff, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC,
			ZBX_DEFAULT_UINT64_COMPARE_FUNC, service_diff_clean, ZBX_DEFAULT_MEM_MALLOC_FUNC,
			ZBX_DEFAULT_MEM_REALLOC_FUNC, ZBX_DEFAULT_MEM_FREE_FUNC);
}

static void	service_manager_free(zbx_service_manager_t *service_manager)
{
	zbx_hashset_destroy(&service_manager->service_problems_index);
	zbx_hashset_destroy(&service_manager->services_links);
	zbx_hashset_destroy(&service_manager->service_problem_tags);
	zbx_hashset_destroy(&service_manager->services);
	zbx_hashset_destroy(&service_manager->problem_events);
	zbx_hashset_destroy(&service_manager->recovery_events);
}

static void	recalculate_services(zbx_service_manager_t *service_manager)
{
	zbx_hashset_iter_t	iter;
	zbx_event_t		**event;
	zbx_service_t		*pservice;
	zbx_services_diff_t	services_diff, *pservices_diff;
	int			flags;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	flags = ZBX_FLAG_SERVICE_RECALCULATE|ZBX_FLAG_SERVICE_DELETE;

	zbx_hashset_iter_reset(&service_manager->problem_events, &iter);
	while (NULL != (event = (zbx_event_t **)zbx_hashset_iter_next(&iter)))
	{
		match_event_to_service_problem_tags(*event, &service_manager->service_problem_tags_index,
				&service_manager->services_diff, flags);
	}

	zbx_hashset_iter_reset(&service_manager->services, &iter);
	while (NULL != (pservice = (zbx_service_t *)zbx_hashset_iter_next(&iter)))
	{
		if (0 != pservice->children.values_num)
			continue;

		services_diff.serviceid = pservice->serviceid;
		pservices_diff = zbx_hashset_search(&service_manager->services_diff, &services_diff);

		if (NULL == pservices_diff)
		{
			zbx_vector_ptr_create(&services_diff.service_problems);
			zbx_vector_ptr_create(&services_diff.service_problems_recovered);
			services_diff.flags = flags;
			zbx_hashset_insert(&service_manager->services_diff, &services_diff,
					sizeof(services_diff));
		}
	}

	db_update_services(&service_manager->services, &service_manager->services_diff,
			&service_manager->service_problems_index);
	zbx_hashset_clear(&service_manager->services_diff);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

ZBX_THREAD_ENTRY(service_manager_thread, args)
{
	zbx_ipc_service_t	service;
	char			*error = NULL;
	zbx_ipc_client_t	*client;
	zbx_ipc_message_t	*message;
	int			ret, processed_num = 0;
	double			time_stat, time_idle = 0, time_now, time_flush = 0, sec;
	zbx_service_manager_t	service_manager;

#define	STAT_INTERVAL	5	/* if a process is busy and does not sleep then update status not faster than */
				/* once in STAT_INTERVAL seconds */

	process_type = ((zbx_thread_args_t *)args)->process_type;
	server_num = ((zbx_thread_args_t *)args)->server_num;
	process_num = ((zbx_thread_args_t *)args)->process_num;

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(program_type),
				server_num, get_process_type_string(process_type), process_num);

	update_selfmon_counter(ZBX_PROCESS_STATE_BUSY);

	zbx_setproctitle("%s #%d [connecting to the database]", get_process_type_string(process_type), process_num);

	DBconnect(ZBX_DB_CONNECT_NORMAL);

	if (FAIL == zbx_ipc_service_start(&service, ZBX_IPC_SERVICE_SERVICE, &error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot start service manager service: %s", error);
		zbx_free(error);
		exit(EXIT_FAILURE);
	}

	/* initialize statistics */
	time_stat = zbx_time();

	service_manager_init(&service_manager);

	DBbegin();
	db_get_events(&service_manager.problem_events);	/* TODO: housekeeping, get events before history syncer starts to recover */
	DBcommit();

	zbx_setproctitle("%s #%d started", get_process_type_string(process_type), process_num);

	while (ZBX_IS_RUNNING())
	{
		time_now = zbx_time();
//
//		if (STAT_INTERVAL < time_now - time_stat)
//		{
//			zbx_setproctitle("%s #%d [queued %d, processed %d values, idle "
//					ZBX_FS_DBL " sec during " ZBX_FS_DBL " sec]",
//					get_process_type_string(process_type), process_num,
//					interface_availabilities.values_num, processed_num, time_idle, time_now - time_stat);
//
//			time_stat = time_now;
//			time_idle = 0;
//			processed_num = 0;
//		}
//
		if (ZBX_SERVICE_MANAGER_SYNC_DELAY_SEC < time_now - time_flush)
		{
			int	updated = 0;

			DBbegin();
			sync_services(&service_manager.services, &updated);
			sync_service_problem_tags(&service_manager);
			sync_services_links(&service_manager);

			if (0 == time_flush)
				sync_service_problems(&service_manager.services, &service_manager.service_problems_index);

			DBcommit();

			if (0 != updated)
				recalculate_services(&service_manager);

			time_flush = time_now;
			time_now = zbx_time();
		}

		update_selfmon_counter(ZBX_PROCESS_STATE_IDLE);
		ret = zbx_ipc_service_recv(&service, 1, &client, &message);
		update_selfmon_counter(ZBX_PROCESS_STATE_BUSY);
		sec = zbx_time();
		zbx_update_env(sec);

		if (ZBX_IPC_RECV_IMMEDIATE != ret)
			time_idle += sec - time_now;

		if (NULL != message)
		{
			zbx_vector_ptr_t	events;

			zbx_vector_ptr_create(&events);

			zbx_service_deserialize(message->data, message->size, &events);
			zbx_ipc_message_free(message);
			process_events(&events, &service_manager);

			zbx_vector_ptr_destroy(&events);
		}

		if (NULL != client)
			zbx_ipc_client_release(client);
	}

	service_manager_free(&service_manager);

	DBclose();

	exit(EXIT_SUCCESS);
#undef STAT_INTERVAL
}

