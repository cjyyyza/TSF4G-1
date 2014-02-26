#include "tsqld.h"
#include <stdio.h>
#include <string.h>
#include "tlog/tlog_print.h"
#include "tlog/tlog_log.h"
#include "tlibc/protocol/tlibc_xml_reader.h"
#include "tsqld/tsqld_config_types.h"
#include "tsqld/tsqld_config_reader.h"
#include "tsqld/tsqld_tbus.h"
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include "tcommon/terrno.h"
#include "tlibc/core/tlibc_hash.h"
#include "mysql.h"

MYSQL *g_mysql = NULL;

#define PROGRAN_NAME "tsqld"
#define TSQLD_VERSION "0.0.1"

tsqld_config_t g_config;
bool g_sig_term;
sql_hash_table_s g_sql_hash_table[TSQLD_SQL_NUM];
tlibc_hash_t g_sql_hash;



static void version()
{
	INFO_PRINT("%s version %s.", PROGRAN_NAME, TSQLD_VERSION);
}

static void help()
{
	INFO_PRINT("Usage: %s [options] file.", PROGRAN_NAME);  
	INFO_PRINT("Options:");
	INFO_PRINT("  -version                 Print the compiler version.");
    INFO_PRINT("  -help                    Print the useage.");
    INFO_PRINT("  -i                       Add include path.");
    INFO_PRINT("  -l                       Set the log config file.");
	INFO_PRINT("  file                     Set the config file.");
}

static void load(int argc, char* argv[])
{
	int i;
	const char* config_file = NULL;
    TLIBC_XML_READER xml_reader;
    tlibc_xml_reader_init(&xml_reader);
    TLIBC_ERROR_CODE r;

	for (i = 1; i < argc; ++i)
	{
		char* arg;

		arg = strtok(argv[i], " ");
		if (arg[0] == '-' && arg[1] == '-')
		{
			++arg;
		}
		if (strcmp(arg, "-help") == 0)
		{
			help();
			exit(0);
		}
		else if (strcmp(arg, "-version") == 0)
		{
			version();
			exit(0);
		}
		else if (strcmp(arg, "-l") == 0)
		{
		    arg = argv[++i];
 
            if(tlog_init(&g_tlog_instance, arg) != E_TS_NOERROR)
            {
                ERROR_PRINT("tlog init [%s] failed.", arg);
                exit(1);
            }
            INFO_PRINT("tlog init(%s) succeed, check the log file for more information.", arg);
        }
		else if (strcmp(arg, "-i") == 0)
		{
		    arg = argv[++i];

			if(tlibc_xml_add_include(&xml_reader, arg) != E_TLIBC_NOERROR)
			{
				ERROR_PRINT("Too many include path [%s].", arg);
				exit(1);
			}
		}
		else
		{
		    config_file = arg;
            break;
		}

		arg = strtok(NULL, " =");
	}

	if (config_file == NULL)
	{
		ERROR_PRINT("Missing config file specification");
		help();
		exit(1);
	}

    if(tlibc_xml_reader_push_file(&xml_reader, config_file) != E_TLIBC_NOERROR)
    {   
        ERROR_PRINT("load push config file [%s] failed.", config_file);
        exit(1);
    }

    r = tlibc_read_tsqld_config_t(&xml_reader.super, &g_config);
	if(r != E_TLIBC_NOERROR)
    {   
        const TLIBC_XML_READER_YYLTYPE *lo = tlibc_xml_current_location(&xml_reader);
        if(lo)
        {   
            ERROR_PRINT("load xml [%s] return [%d] at %d,%d - %d,%d.", lo->file_name, r,
                lo->first_line, lo->first_column, lo->last_line, lo->last_column);
        }   
        else
        {   
	        ERROR_PRINT("load xml [%s] return [%d].", config_file, r);
        }

        tlibc_xml_reader_pop_file(&xml_reader);
		exit(1);
    }
    tlibc_xml_reader_pop_file(&xml_reader);
}

static void on_signal(int sig)
{
    switch(sig)
    {
        case SIGINT:
        case SIGTERM:
            g_sig_term = true;
            break;
    }
}


tlibc_hash_bucket_t buckets[TSQLD_SQL_NUM];

static TERROR_CODE init()
{
    TERROR_CODE ret = E_TS_NOERROR;
    size_t i, cur_stmt;
    const char*password;
    struct sigaction  sa;

    cur_stmt = 0;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = on_signal;
	if(sigemptyset(&sa.sa_mask) != 0)
	{
	    ERROR_LOG("sigemptyset errno[%d], %s.", errno, strerror(errno));
	    ret = E_TS_ERRNO;
        goto done;
	}

	if((sigaction(SIGTERM, &sa, NULL) != 0)
	 ||(sigaction(SIGINT, &sa, NULL) != 0))
	{
        ERROR_LOG("sigaction error[%d], %s.", errno, strerror(errno));
	    ret = E_TS_ERRNO;
        goto done;
	}
	g_sig_term = false;


	ret = tsqld_tbus_init();
	if(ret != E_TS_NOERROR)
	{
	    ret = E_TS_ERRNO;
        goto done;
	}
  
	g_mysql = mysql_init(NULL);
	if(g_mysql == NULL)
	{
        ret = E_TS_ERROR;
        ERROR_LOG("mysql_client_init Error %u: %s", mysql_errno(g_mysql), mysql_error(g_mysql));
        goto done;
	}

    if(g_config.password[0])
    {
        password = g_config.password;
    }
    else
    {
        password = NULL;
    }
    
	if(mysql_real_connect(g_mysql, g_config.host, g_config.user, password, g_config.db
	    , g_config.port, NULL, 0) == NULL)
	{
        ERROR_LOG("mysql_real_connect Error %u: %s", mysql_errno(g_mysql), mysql_error(g_mysql));
        ret = E_TS_ERROR;
        goto close_mysql;
    }


    tlibc_hash_init(&g_sql_hash, buckets, TSQLD_SQL_NUM);
    
    for(cur_stmt = 0; cur_stmt < g_config.sql_vec_num; ++cur_stmt)
    {
        tsqld_sql_vec *sv = &g_config.sql_vec[cur_stmt];
        sql_hash_table_s *s = &g_sql_hash_table[cur_stmt];
        s->stmt = mysql_stmt_init(g_mysql);
        if(s->stmt == NULL)
        {
            ERROR_LOG("mysql_stmt_init Error %u: %s", mysql_errno(g_mysql), mysql_error(g_mysql));
            ret = E_TS_ERROR;
            goto rollback_stmt;
        }

        s->sql = sv->sql;

        if(mysql_stmt_prepare(s->stmt, s->sql, strlen(s->sql)))
        {
            ERROR_LOG("mysql_stmt_prepare Error %u: %s", mysql_errno(g_mysql), mysql_error(g_mysql));
            ret = E_TS_ERROR;
            goto rollback_stmt;
        }

        s->param_count = (uint32_t)mysql_stmt_param_count(s->stmt);
        s->res = mysql_stmt_result_metadata(s->stmt);
        if(s->res)
        {
            //���ڻ᷵�ؽ���Ĳ�ѯ�� ��Ҫ��������Ϣ
            s->field_vec = mysql_fetch_fields(s->res);
            if(s->field_vec == NULL)
            {
                ERROR_LOG("mysql_fetch_fields Error %u: %s", mysql_errno(g_mysql), mysql_error(g_mysql));
                ret = E_TS_ERROR;
                goto rollback_stmt;
            }
            s->field_vec_count = mysql_stmt_field_count(s->stmt);
        }
        else
        {
            s->field_vec = NULL;
        }

        tlibc_hash_insert(&g_sql_hash, sv->name, (uint32_t)strlen(sv->name), &s->entry);
    }
    cur_stmt = 0;
	
	return E_TS_NOERROR;
rollback_stmt:
    for(i = 0; i < cur_stmt; ++i)
    {
        sql_hash_table_s *s = &g_sql_hash_table[i];
        mysql_stmt_close(s->stmt);
    }

close_mysql:
    mysql_close(g_mysql);
done:
    return ret;
}

static void work()
{
    uint32_t idle_count = 0;
    TERROR_CODE ret;

    while(!g_sig_term)
    {
        ret = tsqld_tbus_proc();
        if(ret == E_TS_NOERROR)
        {
            idle_count = 0;
        }
        else if(ret == E_TS_WOULD_BLOCK)
        {
            ++idle_count;
            if(idle_count > 30)
            {
                if((usleep(1 * 1000) != 0) && (errno != EINTR))
                {
                    ERROR_LOG("usleep errno [%d], %s", errno, strerror(errno));
                    goto done;
                }
                idle_count = 0;                 
            }
        }
        else
        {
            goto done;
        }
    }
done:
    return;
}

static void fini()
{
    tsqld_tbus_fini();
}

int main(int argc, char *argv[])
{
	load(argc, argv);

    if(init() != E_TS_NOERROR)
    {
        goto ERROR_RET;
    }

	work();
	
	fini();
	
    return 0;
ERROR_RET:
    return 1;
}
