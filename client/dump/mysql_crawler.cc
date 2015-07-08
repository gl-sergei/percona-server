/*
  Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "mysql_crawler.h"
#include "mysql_function.h"
#include "stored_procedure.h"
#include "table_definition_dump_task.h"
#include "table_rows_dump_task.h"
#include "table_deferred_indexes_dump_task.h"
#include "event_scheduler_event.h"
#include "privilege.h"
#include "trigger.h"
#include "view.h"
#include "base/mysql_query_runner.h"
#include <string>
#include <vector>
using std::string;
using std::vector;

using namespace Mysql::Tools::Dump;

Mysql_crawler::Mysql_crawler(I_connection_provider* connection_provider,
  Mysql::I_callable<bool, const Mysql::Tools::Base::Message_data&>*
    message_handler, Simple_id_generator* object_id_generator,
  Mysql_chain_element_options* options)
  : Abstract_crawler(message_handler, object_id_generator),
  Abstract_mysql_chain_element_extension(
  connection_provider, message_handler, options)
{}

void Mysql_crawler::enumerate_objects()
{
  m_dump_start_task= new Dump_start_dump_task();
  m_dump_end_task= new Dump_end_dump_task();
  m_tables_definition_ready_dump_task=
    new Tables_definition_ready_dump_task();

  this->process_dump_task(m_dump_start_task);

  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();

  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> databases;
  runner->run_query_store("SHOW DATABASES", &databases);

  std::vector<Database* > db_list;
  std::vector<Database_end_dump_task* > db_end_task_list;
  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= databases.begin(); it != databases.end(); ++it)
  {
    std::string db_name= (**it)[0];

    Database* database= new Database(
      this->generate_new_object_id(), db_name,
      this->get_create_statement(runner, "", db_name,
      "DATABASE IF NOT EXISTS").value());

    db_list.push_back(database);
    m_current_database_start_dump_task=
      new Database_start_dump_task(database);
    m_current_database_end_dump_task=
      new Database_end_dump_task(database);
    db_end_task_list.push_back(m_current_database_end_dump_task);

    m_current_database_start_dump_task->add_dependency(m_dump_start_task);
    m_dump_end_task->add_dependency(m_current_database_end_dump_task);

    this->process_dump_task(m_current_database_start_dump_task);
    this->enumerate_database_objects(*database);
    m_current_database_start_dump_task= NULL;
  }

  m_dump_end_task->add_dependency(m_tables_definition_ready_dump_task);
  this->process_dump_task(m_tables_definition_ready_dump_task);

  std::vector<Database* >::iterator it;
  std::vector<Database_end_dump_task* >::iterator it_end;
  for (it= db_list.begin(),it_end= db_end_task_list.begin();
       ((it != db_list.end()) && (it_end != db_end_task_list.end()));
        ++it, ++it_end)
  {
    m_current_database_end_dump_task= *it_end;
    this->enumerate_views(**it);
    this->process_dump_task(m_current_database_end_dump_task);
    m_current_database_end_dump_task= NULL;
  }

  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&databases);

  this->enumerate_users();

  this->process_dump_task(m_dump_end_task);

  this->report_crawler_completed(this);

  this->wait_for_tasks_completion();
}

void Mysql_crawler::enumerate_database_objects(const Database& db)
{
  this->enumerate_tables(db);
  this->enumerate_functions<Mysql_function>(db, "FUNCTION");
  this->enumerate_functions<Stored_procedure>(db, "PROCEDURE");
  this->enumerate_event_scheduler_events(db);
}

void Mysql_crawler::enumerate_tables(const Database& db)
{
  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();
  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> tables;

  runner->run_query_store("SHOW TABLE STATUS FROM "
    + this->quote_name(db.get_name()), &tables);

  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= tables.begin(); it != tables.end(); ++it)
  {
    const Mysql::Tools::Base::Mysql_query_runner::Row& table_data= **it;

    std::string table_name= table_data[0]; // "Name"
    std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> fields_data;
    runner->run_query_store("SHOW COLUMNS IN " + this->quote_name(table_name)
      + " FROM " + this->quote_name(db.get_name()), &fields_data);
    std::vector<Field> fields;
    for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>
      ::iterator field_it=
        fields_data.begin(); field_it != fields_data.end(); ++field_it)
    {
      fields.push_back(Field((**field_it)[0], (**field_it)[1]));
    }
    Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&fields_data);
    /*
      For views create a dummy view so that dependent objects are
      resolved when actually dumping views.
    */

    if (table_data.is_value_null(1))
    {
      std::string fake_view_ddl = "CREATE VIEW "
          + this->get_quoted_object_full_name(db.get_name(), table_name)
          + " AS SELECT\n";

      for (std::vector<Field>::iterator field_iterator= fields.begin();
           field_iterator != fields.end();
           ++field_iterator)
      {
        fake_view_ddl+= " 1 AS " + this->quote_name(field_iterator->get_name());
        if (field_iterator + 1 != fields.end())
          fake_view_ddl += ",\n";
      }

      View* fake_view= new View(this->generate_new_object_id(),
                           table_name,
                           db.get_name(),
                           fake_view_ddl);

      fake_view->add_dependency(m_current_database_start_dump_task);
      m_current_database_end_dump_task->add_dependency(fake_view);
      m_tables_definition_ready_dump_task->add_dependency(fake_view);
      this->process_dump_task(fake_view);
      continue;
    }

    uint64 rows= table_data.is_value_null(4)
      ? 0 : atoll(table_data[4].c_str()); // "Rows"
    bool isInnoDB= table_data[1] == "InnoDB"; // "Engine"
    Table* table= new Table(this->generate_new_object_id(),
      table_name,
      db.get_name(),
      this->get_create_statement(
      runner, db.get_name(), table_name, "TABLE")
      .value(),
      fields, table_data[1], rows,
      (uint64)(rows * (isInnoDB ? 1.5 : 1)),
      atoll(table_data[6].c_str()) // "Data_length"
      );

    Table_definition_dump_task* ddl_task=
      new Table_definition_dump_task(table);
    Table_rows_dump_task* rows_task= new Table_rows_dump_task(table);
    Table_deferred_indexes_dump_task* indexes_task=
      new Table_deferred_indexes_dump_task(table);

    ddl_task->add_dependency(m_current_database_start_dump_task);
    rows_task->add_dependency(ddl_task);
    indexes_task->add_dependency(rows_task);
    m_current_database_end_dump_task->add_dependency(indexes_task);
    m_tables_definition_ready_dump_task->add_dependency(ddl_task);

    this->process_dump_task(ddl_task);
    this->process_dump_task(rows_task);

    this->enumerate_table_triggers(*table, rows_task);

    this->process_dump_task(indexes_task);
  }
  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&tables);
}

void Mysql_crawler::enumerate_views(const Database& db)
{
  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();
  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> tables;

  runner->run_query_store("SHOW TABLES FROM "
    + this->quote_name(db.get_name()), &tables);

  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= tables.begin(); it != tables.end(); ++it)
  {
    const Mysql::Tools::Base::Mysql_query_runner::Row& table_data= **it;

    std::string table_name= table_data[0]; // "View Name"
    std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> check_view;
    runner->run_query_store("SELECT COUNT(*) FROM "
            + this->get_quoted_object_full_name("INFORMATION_SCHEMA", "VIEWS")
            + " WHERE TABLE_NAME= '" + runner->escape_string(table_name)
            + "' AND TABLE_SCHEMA= '" + runner->escape_string(db.get_name()) + "'",
            &check_view);

    for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
         view_it= check_view.begin(); view_it != check_view.end(); ++view_it)
    {
      const Mysql::Tools::Base::Mysql_query_runner::Row& is_view= **view_it;
      if (is_view[0] == "1")
      {
        /* Check if view dependent objects exists */
        if (runner->run_query(std::string("LOCK TABLES ") + db.get_name()
                    + "." + table_name + " READ") != 0)
          return;
        else
          runner->run_query(std::string("UNLOCK TABLES"));
        View* view= new View(this->generate_new_object_id(),
                              table_name,
                              db.get_name(),
                              this->get_create_statement(runner,
                                                         db.get_name(),
                                                         table_name,
                                                         "TABLE").value()
                              );
        m_current_database_end_dump_task->add_dependency(view);
        view->add_dependency(m_tables_definition_ready_dump_task);
        this->process_dump_task(view);
      }
    }
    Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&check_view);
  }
  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&tables);
}

template<typename TObject>
void Mysql_crawler::enumerate_functions(const Database& db, std::string type)
{
  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();

  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> functions;
  runner->run_query_store("SHOW " + type + " STATUS WHERE db = '"
    + runner->escape_string(db.get_name()) + '\'', &functions);

  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= functions.begin(); it != functions.end(); ++it)
  {
    const Mysql::Tools::Base::Mysql_query_runner::Row& function_row= **it;

    TObject* function= new TObject(this->generate_new_object_id(),
      function_row[1], db.get_name(),
      "DELIMITER //\n" + this->get_create_statement(
      runner, db.get_name(), function_row[1], type, 2).value()
      + "//\n" + "DELIMITER ;\n");

    function->add_dependency(m_current_database_start_dump_task);
    m_current_database_end_dump_task->add_dependency(function);

    this->process_dump_task(function);
  }
  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&functions);
}

void Mysql_crawler::enumerate_event_scheduler_events(const Database& db)
{
  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();

  // Workaround for "access denied" error fixed in 5.7.6.
  if (this->get_server_version() < 50706
    && this->compare_no_case_latin_with_db_string(
    "performance_schema", db.get_name()) == 0)
  {
    return;
  }

  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> events;
  runner->run_query_store("SHOW EVENTS FROM "
    + this->get_quoted_object_full_name(&db), &events);

  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= events.begin(); it != events.end(); ++it)
  {
    const Mysql::Tools::Base::Mysql_query_runner::Row& event_row= **it;

    Event_scheduler_event* event=
      new Event_scheduler_event(this->generate_new_object_id(),
      event_row[1], db.get_name(),
      "DELIMITER //\n" + this->get_create_statement(
      runner, db.get_name(), event_row[1], "EVENT", 3).value()
      + "//\n" + "DELIMITER ;\n");

    event->add_dependency(m_current_database_start_dump_task);
    m_current_database_end_dump_task->add_dependency(event);

    this->process_dump_task(event);
  }
  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&events);
}

void Mysql_crawler::enumerate_users()
{
  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();

  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> users;
  runner->run_query_store(
    "SELECT CONCAT(QUOTE(user),'@',QUOTE(host)) FROM mysql.user", &users);

  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= users.begin(); it != users.end(); ++it)
  {
    const Mysql::Tools::Base::Mysql_query_runner::Row& user_row= **it;

    std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> create_user;
    std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> user_grants;
    runner->run_query_store(
      "SHOW CREATE USER " + user_row[0], &create_user);
    runner->run_query_store(
      "SHOW GRANTS FOR " + user_row[0], &user_grants);

    Abstract_dump_task* previous_grant= m_dump_start_task;

    for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>
      ::iterator it1= create_user.begin(), it2 = user_grants.begin();
      it1 != create_user.end(); ++it1, ++it2)
    {
      const Mysql::Tools::Base::Mysql_query_runner::Row& create_row= **it1;
      const Mysql::Tools::Base::Mysql_query_runner::Row& grant_row= **it2;

      std::string user= std::string(create_row[0] + ";\n" + grant_row[0]);

      Privilege* grant=
        new Privilege(
        this->generate_new_object_id(), user_row[0], user);

      grant->add_dependency(previous_grant);
      if (it1+1 == create_user.end())
        m_dump_end_task->add_dependency(grant);
      this->process_dump_task(grant);
      previous_grant= grant;
    }
    Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&create_user);
    Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&user_grants);
  }
  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&users);
}

void Mysql_crawler::enumerate_table_triggers(
  const Table& table, Abstract_dump_task* dependency)
{
  // Triggers were supported since 5.0.9
  if (this->get_server_version() < 50009)
    return;

  Mysql::Tools::Base::Mysql_query_runner* runner= this->get_runner();

  std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*> triggers;
  runner->run_query_store("SHOW TRIGGERS FROM "
    + this->quote_name(table.get_schema()) + " LIKE '"
    + runner->escape_string(table.get_name()) + '\'', &triggers);

  for (std::vector<const Mysql::Tools::Base::Mysql_query_runner::Row*>::iterator
    it= triggers.begin(); it != triggers.end(); ++it)
  {
    const Mysql::Tools::Base::Mysql_query_runner::Row& trigger_row= **it;
    Trigger* trigger= new Trigger(this->generate_new_object_id(),
      trigger_row[0], table.get_schema(),
      this->get_version_specific_statement(
      this->get_create_statement(
      runner, table.get_schema(), trigger_row[0], "TRIGGER", 2).value(),
      "TRIGGER", "50017", "50003"),
      &table);

    trigger->add_dependency(dependency);
    m_current_database_end_dump_task->add_dependency(trigger);

    this->process_dump_task(trigger);
  }
  Mysql::Tools::Base::Mysql_query_runner::cleanup_result(&triggers);
}


std::string Mysql_crawler::get_version_specific_statement(
  std::string create_string, const std::string& keyword,
  std::string main_version, std::string definer_version)
{
  size_t keyword_pos= create_string.find(" " + keyword);
  size_t definer_pos= create_string.find(" DEFINER");
  if (keyword_pos != std::string::npos && definer_pos != std::string::npos
    && definer_pos < keyword_pos)
  {
    create_string.insert(keyword_pos, "*/ /*!" + main_version);
    create_string.insert(definer_pos, "*/ /*!" + definer_version);
  }
  return "/*!" + main_version + ' ' + create_string + " */";
}
