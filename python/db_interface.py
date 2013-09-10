
import MySQLdb
from threading import Semaphore

dbuser = "wwitt"
dbpass = "112233w"
dbhost = "127.0.0.1"
dbname = "wwitt"

class DBInterface():
	def __init__(self):
		self.db = MySQLdb.connect(dbhost,dbuser,dbpass,dbname)
		self._lock = Semaphore(1)
		
	def insert(self,table,dic):
		values = [ "'"+MySQLdb.escape_string(str(x))+"'" for x in dic.values() ]
		query = "INSERT INTO "+table+" (" + (",".join(dic.keys())) + ") VALUES (" + (",".join(values)) + ")"

		self._lock.acquire()
		try:
			ret = None
			cursor = self.db.cursor()
			cursor.execute(query)
			ret = cursor.lastrowid
		except Exception, err:
			pass

		cursor.close()
		self.db.commit()

		self._lock.release()
		return ret
	
	def select(self,table,fields = "*",cond = {}):
		conds =  [ str(x) + "=" + '"'+MySQLdb.escape_string(str(cond[x]))+'"' for x in cond.keys() ]
		query = "SELECT "+fields+" FROM "+table
		if (len(conds) != 0): query += " WHERE " + (" AND ".join(conds))

		self._lock.acquire()
		try:
			cursor = self.db.cursor()
			cursor.execute(query)
			ret = cursor.fetchall()
			cursor.close()
			self.db.commit()
		except:
			pass

		self._lock.release()

		# If only one filed, convert lists of lists to list
		if (len(fields.split(",")) == 1):
			return ( x[0] for x in ret )
		return ret

	def update(self,table,cond = {},values = {}):
		conds =  [ table+"."+str(x) + "=" + '"'+MySQLdb.escape_string(str(cond[x]))  +'"' for x in cond.keys() ]
		vals  =  [ table+"."+str(x) + "=" + '"'+MySQLdb.escape_string(str(values[x]))+'"' for x in values.keys() ]
		query = "UPDATE "+table+" "
		query += " SET " + (" , ".join(vals))
		if (len(conds) != 0): query += " WHERE " + (" AND ".join(conds))

		self._lock.acquire()
		try:
			cursor = self.db.cursor()
			cursor.execute(query)
			cursor.fetchall()
		except:
			pass

		cursor.close()
		self.db.commit()
		self._lock.release()



