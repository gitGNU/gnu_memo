/*
 * Copyright (C) 2008 Jan Stępień
 *
 * This file is part of Memo.
 *
 * Memo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Memo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Memo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libmemo.h>
#include <stdlib.h>
#include <string.h>

typedef struct _memo_database_data {
	int rows;
	int cols;
	void ***data;
} memo_database_data;

memo_database_data *
memo_database_data_init() {
	return calloc(1, sizeof(memo_database_data));
}

void
memo_database_data_free(memo_database_data * data) {
	int i;
	/* TODO: free a string, if a column contains strings */
	for (i = 0; i < data->rows; i++)
		free(data->data[i]);
	free(data);
}

int
memo_database_load(memo_database *db, const char *filename) {
	if ( sqlite3_open(filename, db) ) {
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(*db));
		sqlite3_close(*db);
		return -1;
	}
	return 0;
}

int
memo_database_execute(memo_database db, const char *query,
		memo_database_data *ret) {
	sqlite3_stmt *stmt;
	int rc;
	if ( sqlite3_prepare_v2(db, query, -1, &stmt, NULL) ) {
		fprintf(stderr, "Error parsing SQL query: %s\n", sqlite3_errmsg(db));
		return -1;
	}
	while (1) {
		/* Execute the query. */
		rc = sqlite3_step(stmt);
		/* If everything's fine we can leave the loop. */
		if ( rc == SQLITE_DONE )
			break;
		/* Reallocate the returned data table, as we need memory for a pointer
		 * to the next row. */
		if (ret) {
			ret->data = realloc(ret->data, sizeof(void**)*(ret->rows+1));
			/* If we know the number of columns (i.e. it's not the first row)
			 * allocate memory for a new row. */
			if (ret->rows > 0)
				ret->data[ret->rows] = malloc(sizeof(void*)*ret->cols);
			else
				ret->data[0] = NULL;
		}
		/* If the database has returned some data and the user provided a
		 * place to store it. */
		if ( rc == SQLITE_ROW && ret ) {
			int i, type, cols;
			/* If column count isn't defined yet, it should be counted from
			 * zero. */
			if ( ret->cols == 0 )
				cols = 0;
			i = 0;
			/* While there are columns. */
			while ( ( type = sqlite3_column_type(stmt, i)) != SQLITE_NULL ) {
				/* If we don't know the column count yet (i.e. it's the first row
				 * of results) count columns and reallocate memory for each
				 * column we discover. */
				if ( ret->cols == 0 ) {
					ret->data[ret->rows] = realloc(ret->data[ret->rows],
							sizeof(void*)*(++cols));
				}
				switch (type) {
					case SQLITE_INTEGER:
						ret->data[ret->rows][i] = (void*) sqlite3_column_int(stmt, i);
						break;
					case SQLITE_TEXT:
						/* TODO: copy the string and add it's address to
						 * ret->data. */
						break;
					default:
						/* TODO: maybe an error message...? */
						return -1;
				}
				i++;
			}
			/* Define the column count unless it's already defined. */
			if ( ret->cols == 0 )
				ret->cols = cols;
			/* A row has been added. */
			ret->rows++;
		} else {
			fprintf(stderr, "Error executing statement: %s\n", sqlite3_errmsg(db));
			return -1;
		}
	}
	if ( sqlite3_finalize(stmt) ) {
		fprintf(stderr, "Error finalising statement: %s\n", sqlite3_errmsg(db));
		return -1;
	}
	return 0;
}

int
memo_database_init(memo_database db) {
	const char *words_query = "CREATE TABLE words (id integer, word text, "\
			"positive_answers integer DEFAULT 0, negative_answers integer "\
			"DEFAULT 0, PRIMARY KEY (id), UNIQUE (word) );";
	const char *translations_query = "CREATE TABLE translations (id integer, "\
			"word_id integer, translation_id integer, PRIMARY KEY (id) ); ";
	if ( memo_database_execute(db, words_query, NULL) < 0 ||
			memo_database_execute(db, translations_query, NULL) < 0 )
		return -1;
	return 0;
}

int
memo_database_close(memo_database db) {
	sqlite3_close(db);
	return 0;
}

/**
 * @return 0 if the word doesn't exist in the database, negative values in
 * case of errors, positive value equal to the ID of a word if the word exists
 * in the database.
 */
int
memo_database_get_word_id(memo_database db, const char *word) {
	int retval;
	memo_database_data *results;
	const char *word_sel_templ = "SELECT (id) from words where word == \"%s\";";
	char *query;
	query = malloc(sizeof(char) * (strlen(word_sel_templ)+strlen(word)-1));
	results = memo_database_data_init();
	if (!query || !results)
		return -1;
	sprintf(query, word_sel_templ, word);
	if (memo_database_execute(db, query, results) < 0)
		return -1;
	/*
	 * KEEP IN MIND, that it works fine only if IDs are greater then 0.
	 */
	if ( results->rows < 1 )
		retval = 0;
	else
		retval = (int) results->data[0][0];
	free(query);
	memo_database_data_free(results);
	return retval;
}

int
memo_database_add_word(memo_database db, const char *key, const char *value) {
	const char *words_ins_templ = "INSERT INTO words (word) VALUES (\"%s\");";
	const char *trans_ins_templ = "INSERT INTO translations "\
			"(word_id, translation_id) VALUES (\"%i\", \"%i\");";
	char *query;
	int longer_length, key_len, value_len, key_id, value_id;

	/* TODO: Database queries in the body of this function duplicate the ones
	 * in memo_database_check_translation function. There's no need to perform
	 * them twice. Some kind of cache would be welcome. */

	/* Check whether the pair we're adding isn't in the database already. */
	if (memo_database_check_translation(db, key, value) == 0)
		return -1;

	key_len = strlen(key);
	value_len = strlen(value);
	longer_length = (key_len > value_len) ? key_len : value_len;
	/* strlen(trans_ins_templ) because it's the longest template. */
	query = malloc(sizeof(char) * (strlen(trans_ins_templ)-2+longer_length+1));
	if (!query)
		return -1;

	/* Insert a new word and it's translation to the database unless they
	 * already are there. If one of them is in the words table, insert only
	 * the one which is missing and update the translations table. */

	if ((key_id = memo_database_get_word_id(db, key)) == 0) {
		/* Insert the word. */
		sprintf(query, words_ins_templ, key);
		if (memo_database_execute(db, query, NULL) < 0)
			return -1;

		/* Get newly inserted word's ID. */
		key_id = memo_database_get_word_id(db, key);
	}
	if ((value_id = memo_database_get_word_id(db, value)) == 0) {
		/* Insert the translation. */
		sprintf(query, words_ins_templ, value);
		if (memo_database_execute(db, query, NULL) < 0)
			return -1;

		/* Get newly inserted translation's ID. */
		value_id = memo_database_get_word_id(db, value);
	}

	/* Insert the key pair to the translations table. */
	sprintf(query, trans_ins_templ, key_id, value_id);
	if (memo_database_execute(db, query, NULL) < 0)
		return -1;

	return 0;
}

int
memo_database_check_translation(memo_database db, const char *key,
		const char *value) {
	const char *trans_sel_templ = "SELECT (id) from translations where "\
			"word_id == \"%i\" AND translation_id == \"%i\";";
	char *query;
	int longer_length, key_len, value_len, key_id, value_id;
	memo_database_data *results;

	/* TODO: check return values! */

	key_len = strlen(key);
	value_len = strlen(value);
	longer_length = (key_len > value_len) ? key_len : value_len;
	/* strlen(trans_sel_templ) because it's the longest template. */
	query = malloc(sizeof(char) * (strlen(trans_sel_templ)-2+longer_length+1));

	/* Get key's and value's IDs from the database. */
	if ((key_id = memo_database_get_word_id(db, key)) == 0)
		return 1;
	else if (key_id < 0)
		return -1;
	if ( (value_id = memo_database_get_word_id(db, value)) == 0)
		return 1;
	else if (value_id < 0)
		return -1;

	/* Check whether the ID pair exists in the database. */
	results = memo_database_data_init();
	if (!results)
		return -1;

	sprintf(query, trans_sel_templ, key_id, value_id);
	memo_database_execute(db, query, results);
	if ( results->rows < 1 ) {
		/* Swap the pair and check again. */
		memo_database_data_free(results);
		results = memo_database_data_init();
		if (!results)
			return -1;
		sprintf(query, trans_sel_templ, value_id, key_id);
		memo_database_execute(db, query, results);
		if ( results->rows < 1 )
			return 1;
	}

	memo_database_data_free(results);
	return 0;
}

/*
 * vim:ts=4:noet:tw=78
 */
