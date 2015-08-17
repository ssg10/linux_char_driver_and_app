filename = "/dev/hv_cdev"

txt = open(filename)

print "Open file %r" % filename

print txt.read()

txt.close()

print "Close file %r" % filename 
