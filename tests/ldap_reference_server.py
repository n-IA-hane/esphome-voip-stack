"""Actual ldaptor LDAPv3 server for independent client interoperability tests."""

from ldaptor import interfaces
from ldaptor.inmemory import ReadOnlyInMemoryLDAPEntry
from ldaptor.protocols.ldap.ldapserver import LDAPServer
from twisted.internet import protocol, reactor
from twisted.python import components


class Factory(protocol.ServerFactory):
    protocol = LDAPServer


root = ReadOnlyInMemoryLDAPEntry("", {})
domain = root.addChild("dc=voip", {"objectClass": [b"domain"], "dc": [b"voip"]})
accounts = domain.addChild("ou=accounts", {"objectClass": [b"organizationalUnit"], "ou": [b"accounts"]})
accounts.addChild("uid=reader", {"objectClass": [b"inetOrgPerson"], "uid": [b"reader"], "cn": [b"Reader"],
                                 "sn": [b"Reader"], "userPassword": [b"fixture-password"]})
book = domain.addChild("ou=phonebook", {"objectClass": [b"organizationalUnit"], "ou": [b"phonebook"]})
for uid, name, number, kind in [
    ("device-a", "Alice", "101", "esphome"),
    ("device-b", "Bob", "102", "sip_account"),
    ("device-c", "Office", "103", "browser"),
]:
    book.addChild(f"uid={uid}", {"objectClass": [b"inetOrgPerson"], "uid": [uid.encode()], "cn": [name.encode()],
                                "sn": [name.encode()], "telephoneNumber": [number.encode()], "ou": [kind.encode()],
                                "labeledURI": [f"sip:{number}@127.0.0.1:5060;transport=tcp".encode()]})
components.registerAdapter(lambda factory: factory.root, Factory, interfaces.IConnectedLDAPEntry)
factory = Factory()
factory.root = root
port = reactor.listenTCP(0, factory, interface="127.0.0.1")
print(port.getHost().port, flush=True)
reactor.run(installSignalHandlers=False)
